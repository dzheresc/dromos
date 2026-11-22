// dromos_pkg: message layout and builders, mirroring sw/include/dromos/proto.hpp
// and sw/include/dromos/events.hpp. A message is 32 bytes, little-endian:
// byte i of the message is bits [8*i +: 8] of msg_t.
`default_nettype none

package dromos_pkg;

    typedef logic [255:0] msg_t;

    localparam logic [15:0] FEED = 16'hFFFF;

    // Message types
    localparam logic [7:0] T_NEW_ORDER  = 8'h01;
    localparam logic [7:0] T_CANCEL     = 8'h02;
    localparam logic [7:0] T_REDUCE     = 8'h03;
    localparam logic [7:0] T_ACCEPTED   = 8'h81;
    localparam logic [7:0] T_REJECTED   = 8'h82;
    localparam logic [7:0] T_FILL       = 8'h83;
    localparam logic [7:0] T_CANCELLED  = 8'h84;
    localparam logic [7:0] T_MD_ADD     = 8'hC1;
    localparam logic [7:0] T_MD_EXECUTE = 8'hC2;
    localparam logic [7:0] T_MD_DELETE  = 8'hC3;

    // Reject reasons
    localparam logic [7:0] R_UNKNOWN_SYMBOL = 8'd1;
    localparam logic [7:0] R_PRICE_WINDOW   = 8'd2;
    localparam logic [7:0] R_BAD_QTY        = 8'd3;
    localparam logic [7:0] R_BAD_TYPE       = 8'd4;
    localparam logic [7:0] R_UNKNOWN_HANDLE = 8'd5;
    localparam logic [7:0] R_POOL_FULL      = 8'd6;
    localparam logic [7:0] R_BAD_REDUCE     = 8'd7;
    localparam logic [7:0] R_BAD_SIDE       = 8'd8;
    localparam logic [7:0] R_BAD_TIF        = 8'd9;

    // Cancel reasons
    localparam logic [7:0] C_USER    = 8'd0;
    localparam logic [7:0] C_IOC     = 8'd1;
    localparam logic [7:0] C_REDUCED = 8'd3;

    // Liquidity flag on fills
    localparam logic [7:0] L_MAKER = 8'd0;
    localparam logic [7:0] L_TAKER = 8'd1;

    // ---- inbound field accessors (offsets as in proto.hpp) -------------------
    /* verilator lint_off UNUSEDSIGNAL */
    function automatic logic [7:0]  in_type  (msg_t m); return m[7:0];     endfunction
    function automatic logic [7:0]  in_side  (msg_t m); return m[15:8];    endfunction
    function automatic logic [15:0] in_symbol(msg_t m); return m[31:16];   endfunction
    function automatic logic [31:0] in_price (msg_t m); return m[63:32];   endfunction  // NewOrder
    function automatic logic [31:0] in_qty   (msg_t m); return m[95:64];   endfunction  // NewOrder
    function automatic logic [7:0]  in_tif   (msg_t m); return m[103:96];  endfunction  // NewOrder
    function automatic logic [31:0] in_new_qty(msg_t m); return m[63:32];  endfunction  // Reduce
    function automatic logic [63:0] in_handle(msg_t m); return m[127:64];  endfunction  // Cancel/Reduce
    function automatic logic [63:0] in_cl_id (msg_t m); return m[191:128]; endfunction  // all inbound
    /* verilator lint_on UNUSEDSIGNAL */

    function automatic logic [63:0] make_handle(logic [7:0] gen, logic [15:0] slot);
        return {40'd0, gen, slot};
    endfunction

    // ---- outbound builders (events.hpp) ----------------------------------------
    function automatic msg_t m_accepted(logic [7:0] side, logic [15:0] sym, logic [31:0] px,
                                        logic [63:0] h, logic [63:0] cl_id, logic [63:0] ts);
        msg_t m = '0;
        m[7:0] = T_ACCEPTED; m[15:8] = side; m[31:16] = sym; m[63:32] = px;
        m[127:64] = h; m[191:128] = cl_id; m[255:192] = ts;
        return m;
    endfunction

    function automatic msg_t m_rejected(logic [7:0] reason, logic [15:0] sym, logic [7:0] orig_type,
                                        logic [63:0] cl_id, logic [63:0] ts);
        msg_t m = '0;
        m[7:0] = T_REJECTED; m[15:8] = reason; m[31:16] = sym; m[39:32] = orig_type;
        m[191:128] = cl_id; m[255:192] = ts;
        return m;
    endfunction

    function automatic msg_t m_fill(logic [7:0] liq, logic [15:0] sym, logic [31:0] px,
                                    logic [63:0] h, logic [31:0] match_id, logic [31:0] qty,
                                    logic [31:0] leaves);
        msg_t m = '0;
        m[7:0] = T_FILL; m[15:8] = liq; m[31:16] = sym; m[63:32] = px; m[127:64] = h;
        m[159:128] = match_id; m[191:160] = qty; m[223:192] = leaves;
        return m;
    endfunction

    function automatic msg_t m_cancelled(logic [7:0] reason, logic [15:0] sym, logic [31:0] qty,
                                         logic [63:0] h, logic [63:0] cl_id, logic [63:0] ts);
        msg_t m = '0;
        m[7:0] = T_CANCELLED; m[15:8] = reason; m[31:16] = sym; m[63:32] = qty;
        m[127:64] = h; m[191:128] = cl_id; m[255:192] = ts;
        return m;
    endfunction

    function automatic msg_t m_md_add(logic [7:0] side, logic [15:0] sym, logic [31:0] px,
                                      logic [63:0] h, logic [31:0] qty, logic [63:0] ts);
        msg_t m = '0;
        m[7:0] = T_MD_ADD; m[15:8] = side; m[31:16] = sym; m[63:32] = px; m[127:64] = h;
        m[159:128] = qty; m[255:192] = ts;
        return m;
    endfunction

    function automatic msg_t m_md_execute(logic [7:0] side, logic [15:0] sym, logic [31:0] px,
                                          logic [63:0] h, logic [31:0] qty, logic [31:0] match_id,
                                          logic [63:0] ts);
        msg_t m = '0;
        m[7:0] = T_MD_EXECUTE; m[15:8] = side; m[31:16] = sym; m[63:32] = px; m[127:64] = h;
        m[159:128] = qty; m[191:160] = match_id; m[255:192] = ts;
        return m;
    endfunction

    function automatic msg_t m_md_delete(logic [7:0] side, logic [15:0] sym, logic [31:0] remaining,
                                         logic [63:0] h, logic [63:0] ts);
        msg_t m = '0;
        m[7:0] = T_MD_DELETE; m[15:8] = side; m[31:16] = sym; m[63:32] = remaining;
        m[127:64] = h; m[255:192] = ts;
        return m;
    endfunction

    // ---- priority encoders --------------------------------------------------------
    function automatic logic [5:0] ffs_lo64(logic [63:0] v);
        logic [5:0] r = '0;
        for (int i = 63; i >= 0; i--) if (v[i]) r = 6'(i);
        return r;
    endfunction

    function automatic logic [5:0] ffs_hi64(logic [63:0] v);
        logic [5:0] r = '0;
        for (int i = 0; i < 64; i++) if (v[i]) r = 6'(i);
        return r;
    endfunction

endpackage

`default_nettype wire
