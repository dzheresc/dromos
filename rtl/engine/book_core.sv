// book_core: the Dromos matching engine core, one message at a time.
//
// Functionally identical to RefEngine (sw/src/refengine.cpp): for the same
// input stream it must emit the same (dest, 32-byte message) sequence; the
// testbench sim/verilator/tb_book_core.cpp checks exactly that.
//
// Structures (as in the design plan):
//   pool_mem   order pool, one entry per resting order, doubly linked per level
//   free_mem   FIFO of free pool slots (pop on add, push on release)
//   lvl_mem    level table: head, tail, qty, count per (symbol, side, level)
//   words_mem  occupancy bitmap, 64 x 64-bit words per book side
//   summary    one 64-bit summary word per book side, in registers
// Every memory has one port and a registered read (BRAM/URAM friendly): an
// address issued in state S returns data in the next state.
//
// v1 is a plain multi-cycle FSM: correctness first. A resting add takes about
// 10 cycles, each fill about 8. Pipelining comes after the RTL matches the
// model on hardware.
//
// Back-pressure: out_valid/out_ready. While the output register is full and
// not taken, the whole FSM (including memory reads and writes) is frozen.
`default_nettype none

module book_core
    import dromos_pkg::*;
#(
    parameter int NSYM = 4,      // symbols, power of two
    parameter int POOL = 65536,  // order pool slots, power of two, <= 65536
    localparam int LEVELS = 4096,
    localparam int LW = 12,                  // level index width
    localparam int SW = $clog2(POOL),        // slot width
    localparam int PW = SW + 1,              // pointer: MSB set = nil
    localparam int YW = (NSYM > 1) ? $clog2(NSYM) : 1,
    localparam int BW = YW + 1               // book index: {symbol, side}
) (
    input  wire logic        clk,
    input  wire logic        rst,

    // Symbol configuration: enable a symbol with its window base.
    input  wire logic        cfg_valid,
    input  wire logic [15:0] cfg_symbol,
    input  wire logic [31:0] cfg_base,

    // Inbound messages, already ordered and stamped by the ingress arbiter.
    input  wire logic        in_valid,
    output logic             in_ready,
    input  wire msg_t        in_msg,
    input  wire logic [15:0] in_session,
    input  wire logic [63:0] in_ts,

    // Outbound events.
    output logic             out_valid,
    input  wire logic        out_ready,
    output logic [15:0]      out_dest,
    output msg_t             out_msg,

    output logic             busy,       // a message is in flight or output pending
    output logic             init_done
);

    // ------------------------------------------------------------------ types
    typedef struct packed {
        logic          live;
        logic [7:0]    gen;
        logic [15:0]   sym;
        logic [15:0]   sess;
        logic          side;      // 0 buy, 1 sell
        logic [LW-1:0] level;
        logic [31:0]   qty;       // leaves
        logic [PW-1:0] prev;
        logic [PW-1:0] next;
        logic [63:0]   cl_id;
    } pe_t;

    typedef struct packed {
        logic [PW-1:0] head;
        logic [PW-1:0] tail;
        logic [31:0]   qty;
        logic [31:0]   count;
    } le_t;

    localparam logic [PW-1:0] NIL = {1'b1, {SW{1'b0}}};
    localparam int LVL_ENTRIES   = NSYM * 2 * LEVELS;
    localparam int WORD_ENTRIES  = NSYM * 2 * 64;
    localparam int INIT_N        = (POOL > LVL_ENTRIES) ? POOL : LVL_ENTRIES;
    localparam int IW            = $clog2(INIT_N + 1);

    function automatic logic is_nil(logic [PW-1:0] p); return p[PW-1]; endfunction
    function automatic logic [PW-1:0] ptr(logic [SW-1:0] s); return {1'b0, s}; endfunction

    // --------------------------------------------------------------- memories
    pe_t           pool_mem  [POOL];
    logic [SW-1:0] free_mem  [POOL];
    le_t           lvl_mem   [LVL_ENTRIES];
    logic [63:0]   words_mem [WORD_ENTRIES];
    logic [63:0]   summary   [NSYM * 2];

    logic [SW-1:0]       pool_addr;  logic pool_we;  pe_t         pool_wdata;  pe_t         pool_rdata;
    logic [SW-1:0]       free_addr;  logic free_we;  logic [SW-1:0] free_wdata; logic [SW-1:0] free_rdata;
    logic [BW+LW-1:0]    lvl_addr;   logic lvl_we;   le_t         lvl_wdata;   le_t         lvl_rdata;
    logic [BW+6-1:0]     words_addr; logic words_we; logic [63:0] words_wdata; logic [63:0] words_rdata;

    wire stall = out_valid && !out_ready;

    always_ff @(posedge clk) begin
        if (!stall) begin
            if (pool_we)  pool_mem[pool_addr]   <= pool_wdata;
            pool_rdata  <= pool_mem[pool_addr];
            if (free_we)  free_mem[free_addr]   <= free_wdata;
            free_rdata  <= free_mem[free_addr];
            if (lvl_we)   lvl_mem[lvl_addr]     <= lvl_wdata;
            lvl_rdata   <= lvl_mem[lvl_addr];
            if (words_we) words_mem[words_addr] <= words_wdata;
            words_rdata <= words_mem[words_addr];
        end
    end

    // ------------------------------------------------------------------ state
    typedef enum logic [5:0] {
        S_INIT, S_IDLE, S_DECODE,
        S_NCHK, S_NALLOC, S_NACC,
        S_MBEST, S_MBEST2, S_MLVL, S_MHEAD, S_MFILL, S_MFILL2, S_MFILL3,
        S_MPART, S_MREM, S_MREM2, S_MREM3, S_MCLR,
        S_NEND, S_RLINK, S_RLINK2, S_RFIX, S_RFIX2, S_RLVL, S_RBIT, S_RADD,
        S_CCHK, S_CLOOK, S_RDLVL, S_RDLVL2, S_RDMD,
        S_CXLVL, S_CX1, S_CX2, S_CX3, S_CX4, S_CX5, S_CXBIT, S_CXOUT, S_CXOUT2
    } st_t;

    st_t st;

    // Latched message.
    msg_t        m;
    logic [15:0] sess;
    logic [63:0] ts;

    // Symbol table.
    logic        sym_en   [NSYM];
    logic [31:0] sym_base [NSYM];

    // Free list.
    logic [SW-1:0] free_head;
    logic [SW:0]   free_count;

    // Working registers.
    logic [IW-1:0] init_i;
    logic [SW-1:0] t_slot;     // taker (incoming order)
    logic [7:0]    t_gen;
    logic [31:0]   t_qty;
    logic [LW-1:0] t_lvl;
    pe_t           o;          // maker / order being cancelled
    logic [SW-1:0] o_slot;
    le_t           l;          // level being worked on
    logic [LW-1:0] best;
    logic [5:0]    best_w;
    logic [63:0]   best_word;
    logic [31:0]   fq;         // fill quantity
    logic [31:0]   match_id;

    // Decoded fields of the latched message.
    wire [7:0]    f_type   = in_type(m);
    wire [15:0]   f_sym    = in_symbol(m);
    wire          f_side   = m[8];
    wire [31:0]   f_price  = in_price(m);
    wire [63:0]   f_handle = in_handle(m);
    wire [63:0]   f_cl_id  = in_cl_id(m);
    wire [SW-1:0] f_slot   = f_handle[SW-1:0];
    wire [YW-1:0] f_y      = f_sym[YW-1:0];

    wire [31:0]   base_sel = sym_base[f_y];
    wire          sym_ok   = (f_sym < 16'(NSYM)) && sym_en[f_y];
    wire          buy      = (f_side == 1'b0);
    wire [BW-1:0] b_tk     = {f_y, f_side};          // taker's own side
    wire [BW-1:0] b_op     = {f_y, ~f_side};         // opposite side
    wire [BW-1:0] b_o      = {f_y, o.side};          // side of order o
    wire [63:0]   sum_op   = summary[b_op];
    wire [5:0]    w_sel    = buy ? ffs_lo64(sum_op) : ffs_hi64(sum_op);
    wire [31:0]   px_best  = base_sel + 32'(best);
    wire [63:0]   t_handle = make_handle(t_gen, 16'(t_slot));
    wire [63:0]   o_handle = make_handle(o.gen, 16'(o_slot));
    wire [7:0]    side8    = {7'd0, f_side};
    wire [7:0]    o_side8  = {7'd0, o.side};

    assign in_ready  = (st == S_IDLE) && !stall;
    assign busy      = (st != S_IDLE) || out_valid;
    assign init_done = (st != S_INIT);

    // ---------------------------------------------- memory requests (comb.)
    always_comb begin
        pool_addr  = '0; pool_we  = 1'b0; pool_wdata  = '0;
        free_addr  = '0; free_we  = 1'b0; free_wdata  = '0;
        lvl_addr   = '0; lvl_we   = 1'b0; lvl_wdata   = '0;
        words_addr = '0; words_we = 1'b0; words_wdata = '0;

        unique case (st)
            S_INIT: begin
                pool_addr = SW'(init_i);  pool_we = (init_i < IW'(POOL));
                free_addr = SW'(init_i);  free_we = (init_i < IW'(POOL));  free_wdata = SW'(init_i);
                lvl_addr  = (BW+LW)'(init_i); lvl_we = (init_i < IW'(LVL_ENTRIES));
                lvl_wdata = '{head: NIL, tail: NIL, qty: '0, count: '0};
                words_addr = (BW+6)'(init_i); words_we = (init_i < IW'(WORD_ENTRIES));
            end

            S_NCHK:   free_addr = free_head;
            S_NALLOC: pool_addr = free_rdata;

            S_MBEST:  words_addr = {b_op, w_sel};
            S_MLVL:   lvl_addr   = {b_op, best};
            S_MHEAD:  pool_addr  = lvl_rdata.head[SW-1:0];

            S_MPART: begin
                pool_addr = o_slot; pool_we = 1'b1; pool_wdata = o;
                lvl_addr = {b_op, best}; lvl_we = 1'b1; lvl_wdata = l;
            end
            S_MREM:   pool_addr = o.next[SW-1:0];
            S_MREM2: begin
                pool_addr = o.next[SW-1:0]; pool_we = 1'b1;
                pool_wdata = pool_rdata; pool_wdata.prev = NIL;
            end
            S_MREM3: begin
                lvl_addr = {b_op, best}; lvl_we = 1'b1; lvl_wdata = l;
                pool_addr = o_slot; pool_we = 1'b1;
                pool_wdata = o; pool_wdata.live = 1'b0; pool_wdata.gen = o.gen + 8'd1;
                free_addr = free_head + SW'(free_count); free_we = 1'b1; free_wdata = o_slot;
            end
            S_MCLR: begin
                words_addr = {b_op, best_w}; words_we = 1'b1;
                words_wdata = best_word & ~(64'd1 << best[5:0]);
            end

            S_NEND: begin
                if (t_qty == 0 || m[96]) begin  // filled, or IOC remainder: release
                    pool_addr = t_slot; pool_we = 1'b1;
                    pool_wdata = '0; pool_wdata.gen = t_gen + 8'd1;
                    pool_wdata.prev = NIL; pool_wdata.next = NIL;
                    free_addr = free_head + SW'(free_count); free_we = 1'b1; free_wdata = t_slot;
                end else begin
                    lvl_addr = {b_tk, t_lvl};
                end
            end
            S_RLINK2: begin
                pool_addr = t_slot; pool_we = 1'b1;
                pool_wdata = '{live: 1'b1, gen: t_gen, sym: f_sym, sess: sess, side: f_side,
                               level: t_lvl, qty: t_qty, prev: l.tail, next: NIL, cl_id: f_cl_id};
            end
            S_RFIX:   pool_addr = l.tail[SW-1:0];
            S_RFIX2: begin
                pool_addr = l.tail[SW-1:0]; pool_we = 1'b1;
                pool_wdata = pool_rdata; pool_wdata.next = ptr(t_slot);
            end
            S_RLVL: begin
                lvl_addr = {b_tk, t_lvl}; lvl_we = 1'b1;
                lvl_wdata = '{head: is_nil(l.tail) ? ptr(t_slot) : l.head, tail: ptr(t_slot),
                              qty: l.qty + t_qty, count: l.count + 32'd1};
                words_addr = {b_tk, t_lvl[LW-1:6]};
            end
            S_RBIT: begin
                words_addr = {b_tk, t_lvl[LW-1:6]}; words_we = 1'b1;
                words_wdata = words_rdata | (64'd1 << t_lvl[5:0]);
            end

            S_CCHK:   pool_addr = f_slot;
            S_RDLVL: begin
                lvl_addr = {b_o, o.level};
                pool_addr = o_slot; pool_we = 1'b1;
                pool_wdata = o; pool_wdata.qty = in_new_qty(m);
            end
            S_RDLVL2: begin
                lvl_addr = {b_o, o.level}; lvl_we = 1'b1;
                lvl_wdata = lvl_rdata; lvl_wdata.qty = lvl_rdata.qty - (o.qty - in_new_qty(m));
            end

            S_CXLVL:  lvl_addr = {b_o, o.level};
            S_CX1:    pool_addr = o.prev[SW-1:0];
            S_CX2: begin
                pool_addr = o.prev[SW-1:0]; pool_we = 1'b1;
                pool_wdata = pool_rdata; pool_wdata.next = o.next;
            end
            S_CX3:    pool_addr = o.next[SW-1:0];
            S_CX4: begin
                pool_addr = o.next[SW-1:0]; pool_we = 1'b1;
                pool_wdata = pool_rdata; pool_wdata.prev = o.prev;
            end
            S_CX5: begin
                lvl_addr = {b_o, o.level}; lvl_we = 1'b1; lvl_wdata = l;
                words_addr = {b_o, o.level[LW-1:6]};
                pool_addr = o_slot; pool_we = 1'b1;
                pool_wdata = o; pool_wdata.live = 1'b0; pool_wdata.gen = o.gen + 8'd1;
                free_addr = free_head + SW'(free_count); free_we = 1'b1; free_wdata = o_slot;
            end
            S_CXBIT: begin
                words_addr = {b_o, o.level[LW-1:6]}; words_we = 1'b1;
                words_wdata = words_rdata & ~(64'd1 << o.level[5:0]);
            end

            default: ;
        endcase
    end

    // ------------------------------------------------------- FSM (registers)
    task automatic emit(input logic [15:0] dest, input msg_t msg);
        out_valid <= 1'b1;
        out_dest  <= dest;
        out_msg   <= msg;
    endtask

    always_ff @(posedge clk) begin
        if (rst) begin
            st         <= S_INIT;
            init_i     <= '0;
            out_valid  <= 1'b0;
            free_head  <= '0;
            free_count <= '0;
            match_id   <= '0;
            for (int i = 0; i < NSYM; i++) begin
                sym_en[i]   <= 1'b0;
                sym_base[i] <= '0;
            end
            for (int i = 0; i < NSYM * 2; i++) summary[i] <= '0;
        end else begin
            if (cfg_valid && cfg_symbol < 16'(NSYM)) begin
                sym_en[cfg_symbol[YW-1:0]]   <= 1'b1;
                sym_base[cfg_symbol[YW-1:0]] <= cfg_base;
            end

            if (out_valid && out_ready) out_valid <= 1'b0;

            if (!stall) begin
                unique case (st)
                    S_INIT: begin
                        init_i <= init_i + 1'b1;
                        if (init_i == IW'(INIT_N - 1)) begin
                            st         <= S_IDLE;
                            free_head  <= '0;
                            free_count <= (SW+1)'(POOL);
                        end
                    end

                    S_IDLE: if (in_valid) begin
                        m    <= in_msg;
                        sess <= in_session;
                        ts   <= in_ts;
                        st   <= S_DECODE;
                    end

                    S_DECODE: begin
                        if (f_type == T_NEW_ORDER)
                            st <= S_NCHK;
                        else if (f_type == T_CANCEL || f_type == T_REDUCE)
                            st <= S_CCHK;
                        else begin
                            emit(sess, m_rejected(R_BAD_TYPE, f_sym, f_type, f_cl_id, ts));
                            st <= S_IDLE;
                        end
                    end

                    // ---------------------------------------------- new order
                    S_NCHK: begin
                        logic [7:0] r;
                        r = 8'd0;
                        if (!sym_ok)                              r = R_UNKNOWN_SYMBOL;
                        else if (in_side(m) > 8'd1)               r = R_BAD_SIDE;
                        else if (in_tif(m) > 8'd1)                r = R_BAD_TIF;
                        else if (in_qty(m) == 32'd0)              r = R_BAD_QTY;
                        else if (f_price < base_sel ||
                                 f_price - base_sel >= 32'(LEVELS)) r = R_PRICE_WINDOW;
                        else if (free_count == 0)                 r = R_POOL_FULL;
                        if (r != 8'd0) begin
                            emit(sess, m_rejected(r, f_sym, T_NEW_ORDER, f_cl_id, ts));
                            st <= S_IDLE;
                        end else begin
                            t_qty <= in_qty(m);
                            t_lvl <= LW'(f_price - base_sel);
                            st    <= S_NALLOC;
                        end
                    end

                    S_NALLOC: begin
                        t_slot     <= free_rdata;
                        free_head  <= free_head + 1'b1;
                        free_count <= free_count - 1'b1;
                        st         <= S_NACC;
                    end

                    S_NACC: begin
                        t_gen <= pool_rdata.gen;
                        emit(sess, m_accepted(side8, f_sym, f_price,
                                              make_handle(pool_rdata.gen, 16'(t_slot)), f_cl_id, ts));
                        st <= S_MBEST;
                    end

                    // ---------------------------------------------- matching
                    S_MBEST: begin
                        best_w <= w_sel;
                        st     <= (sum_op == '0) ? S_NEND : S_MBEST2;
                    end

                    S_MBEST2: begin
                        logic [LW-1:0] bl;
                        bl = {best_w, buy ? ffs_lo64(words_rdata) : ffs_hi64(words_rdata)};
                        best      <= bl;
                        best_word <= words_rdata;
                        st <= (buy ? (bl > t_lvl) : (bl < t_lvl)) ? S_NEND : S_MLVL;
                    end

                    S_MLVL:  st <= S_MHEAD;

                    S_MHEAD: begin
                        l  <= lvl_rdata;
                        st <= S_MFILL;
                    end

                    S_MFILL: begin
                        logic [31:0] q;
                        q = (t_qty < pool_rdata.qty) ? t_qty : pool_rdata.qty;
                        o        <= pool_rdata;
                        o.qty    <= pool_rdata.qty - q;
                        o_slot   <= l.head[SW-1:0];
                        fq       <= q;
                        t_qty    <= t_qty - q;
                        l.qty    <= l.qty - q;
                        match_id <= match_id + 1'b1;
                        emit(pool_rdata.sess,
                             m_fill(L_MAKER, f_sym, px_best,
                                    make_handle(pool_rdata.gen, 16'(l.head[SW-1:0])),
                                    match_id + 1'b1, q, pool_rdata.qty - q));
                        st <= S_MFILL2;
                    end

                    S_MFILL2: begin
                        emit(sess, m_fill(L_TAKER, f_sym, px_best, t_handle, match_id, fq, t_qty));
                        st <= S_MFILL3;
                    end

                    S_MFILL3: begin
                        emit(FEED, m_md_execute({7'd0, ~f_side}, f_sym, px_best, o_handle, fq,
                                                match_id, ts));
                        st <= (o.qty == 0) ? S_MREM : S_MPART;
                    end

                    S_MPART: st <= S_NEND;  // taker is exhausted; maker keeps its place

                    S_MREM: begin  // maker filled: pop it off the head of the level
                        l.head  <= o.next;
                        l.count <= l.count - 1'b1;
                        if (is_nil(o.next)) l.tail <= NIL;
                        st <= is_nil(o.next) ? S_MREM3 : S_MREM2;
                    end

                    S_MREM2: st <= S_MREM3;

                    S_MREM3: begin
                        free_count <= free_count + 1'b1;
                        if (l.count == 0)
                            st <= S_MCLR;
                        else
                            st <= (t_qty != 0) ? S_MBEST : S_NEND;
                    end

                    S_MCLR: begin
                        summary[b_op][best_w] <= |(best_word & ~(64'd1 << best[5:0]));
                        st <= (t_qty != 0) ? S_MBEST : S_NEND;
                    end

                    // ---------------------------------------------- end of a new order
                    S_NEND: begin
                        if (t_qty == 0) begin
                            free_count <= free_count + 1'b1;
                            st <= S_IDLE;
                        end else if (m[96]) begin  // IOC
                            free_count <= free_count + 1'b1;
                            emit(sess, m_cancelled(C_IOC, f_sym, t_qty, t_handle, f_cl_id, ts));
                            st <= S_IDLE;
                        end else begin
                            st <= S_RLINK;
                        end
                    end

                    S_RLINK: begin
                        l  <= lvl_rdata;
                        st <= S_RLINK2;
                    end

                    S_RLINK2: st <= is_nil(l.tail) ? S_RLVL : S_RFIX;
                    S_RFIX:   st <= S_RFIX2;
                    S_RFIX2:  st <= S_RLVL;

                    S_RLVL: begin
                        st <= (l.count == 0) ? S_RBIT : S_RADD;
                    end

                    S_RBIT: begin
                        summary[b_tk][t_lvl[LW-1:6]] <= 1'b1;
                        st <= S_RADD;
                    end

                    S_RADD: begin
                        emit(FEED, m_md_add(side8, f_sym, f_price, t_handle, t_qty, ts));
                        st <= S_IDLE;
                    end

                    // ---------------------------------------------- cancel / reduce
                    S_CCHK: begin
                        if (!sym_ok) begin
                            emit(sess, m_rejected(R_UNKNOWN_SYMBOL, f_sym, f_type, f_cl_id, ts));
                            st <= S_IDLE;
                        end else if (f_handle[63:24] != '0 || 32'(f_handle[15:0]) >= 32'(POOL)) begin
                            emit(sess, m_rejected(R_UNKNOWN_HANDLE, f_sym, f_type, f_cl_id, ts));
                            st <= S_IDLE;
                        end else begin
                            st <= S_CLOOK;
                        end
                    end

                    S_CLOOK: begin
                        o      <= pool_rdata;
                        o_slot <= f_slot;
                        if (!pool_rdata.live || pool_rdata.gen != f_handle[23:16] ||
                            pool_rdata.sym != f_sym || pool_rdata.sess != sess) begin
                            emit(sess, m_rejected(R_UNKNOWN_HANDLE, f_sym, f_type, f_cl_id, ts));
                            st <= S_IDLE;
                        end else if (f_type == T_REDUCE) begin
                            if (in_new_qty(m) == 0 || in_new_qty(m) >= pool_rdata.qty) begin
                                emit(sess, m_rejected(R_BAD_REDUCE, f_sym, f_type, f_cl_id, ts));
                                st <= S_IDLE;
                            end else begin
                                st <= S_RDLVL;
                            end
                        end else begin
                            st <= S_CXLVL;
                        end
                    end

                    S_RDLVL: st <= S_RDLVL2;

                    S_RDLVL2: begin
                        emit(sess, m_cancelled(C_REDUCED, f_sym, o.qty - in_new_qty(m), f_handle,
                                               o.cl_id, ts));
                        st <= S_RDMD;
                    end

                    S_RDMD: begin
                        emit(FEED, m_md_delete(o_side8, f_sym, in_new_qty(m), f_handle, ts));
                        st <= S_IDLE;
                    end

                    S_CXLVL: st <= S_CX1;

                    S_CX1: begin
                        l       <= lvl_rdata;
                        l.qty   <= lvl_rdata.qty - o.qty;
                        l.count <= lvl_rdata.count - 1'b1;
                        if (is_nil(o.prev)) l.head <= o.next;
                        if (is_nil(o.next)) l.tail <= o.prev;
                        st <= is_nil(o.prev) ? S_CX3 : S_CX2;
                    end

                    S_CX2: st <= S_CX3;
                    S_CX3: st <= is_nil(o.next) ? S_CX5 : S_CX4;
                    S_CX4: st <= S_CX5;

                    S_CX5: begin
                        free_count <= free_count + 1'b1;
                        st <= (l.count == 0) ? S_CXBIT : S_CXOUT;
                    end

                    S_CXBIT: begin
                        summary[b_o][o.level[LW-1:6]] <= |(words_rdata & ~(64'd1 << o.level[5:0]));
                        st <= S_CXOUT;
                    end

                    S_CXOUT: begin
                        emit(sess, m_cancelled(C_USER, f_sym, o.qty, f_handle, o.cl_id, ts));
                        st <= S_CXOUT2;
                    end

                    S_CXOUT2: begin
                        emit(FEED, m_md_delete(o_side8, f_sym, 32'd0, f_handle, ts));
                        st <= S_IDLE;
                    end

                    default: st <= S_IDLE;
                endcase
            end
        end
    end

endmodule

`default_nettype wire
