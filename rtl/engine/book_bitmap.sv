// book_bitmap: occupancy bitmap of one book side, with best-price search.
//
// LEVELS bits, stored as WORDS 64-bit words plus a WORDS-bit summary word
// (summary[w] = |words[w]). One set/clear per cycle. The lowest and highest
// set levels are found by two pipelined priority encoders:
//   stage 1: priority-encode the summary  -> word index
//   stage 2: priority-encode that word    -> bit index
// so best_* reflect the bitmap as it was 2 cycles earlier (LATENCY = 2).
// Asks use best_lo, bids use best_hi.
//
// Mirrors RefEngine::set_bit / clear_bit / lowest / highest.

`default_nettype none

module book_bitmap #(
    parameter int LEVELS = 4096,
    localparam int WORDS = LEVELS / 64,
    localparam int LW    = $clog2(LEVELS),
    localparam int WW    = (WORDS > 1) ? $clog2(WORDS) : 1
) (
    input  wire logic          clk,
    input  wire logic          rst,

    input  wire logic          upd_valid,
    input  wire logic          upd_set,    // 1 = set, 0 = clear
    input  wire logic [LW-1:0] upd_level,

    output logic               best_lo_valid,
    output logic [LW-1:0]      best_lo,
    output logic               best_hi_valid,
    output logic [LW-1:0]      best_hi
);

    logic [63:0]      words [WORDS];
    logic [WORDS-1:0] summary;

    // ---- update -------------------------------------------------------------
    wire [WW-1:0] upd_w = upd_level[LW-1:6];
    wire [5:0]    upd_b = upd_level[5:0];

    always_ff @(posedge clk) begin
        if (rst) begin
            for (int w = 0; w < WORDS; w++) words[w] <= '0;
            summary <= '0;
        end else if (upd_valid) begin
            logic [63:0] nw;
            nw = words[upd_w];
            nw[upd_b] = upd_set;
            words[upd_w]   <= nw;
            summary[upd_w] <= |nw;
        end
    end

    // ---- priority encoders ----------------------------------------------------
    function automatic logic [WW-1:0] lowest_w(input logic [WORDS-1:0] v);
        lowest_w = '0;
        for (int i = WORDS - 1; i >= 0; i--) if (v[i]) lowest_w = WW'(i);
    endfunction

    function automatic logic [WW-1:0] highest_w(input logic [WORDS-1:0] v);
        highest_w = '0;
        for (int i = 0; i < WORDS; i++) if (v[i]) highest_w = WW'(i);
    endfunction

    function automatic logic [5:0] lowest_b(input logic [63:0] v);
        lowest_b = '0;
        for (int i = 63; i >= 0; i--) if (v[i]) lowest_b = 6'(i);
    endfunction

    function automatic logic [5:0] highest_b(input logic [63:0] v);
        highest_b = '0;
        for (int i = 0; i < 64; i++) if (v[i]) highest_b = 6'(i);
    endfunction

    // ---- stage 1: pick the word, capture it -----------------------------------
    logic          s1_any;
    logic [WW-1:0] s1_lo_w, s1_hi_w;
    logic [63:0]   s1_lo_word, s1_hi_word;

    always_ff @(posedge clk) begin
        if (rst) begin
            s1_any <= 1'b0;
        end else begin
            s1_any     <= |summary;
            s1_lo_w    <= lowest_w(summary);
            s1_hi_w    <= highest_w(summary);
            s1_lo_word <= words[lowest_w(summary)];
            s1_hi_word <= words[highest_w(summary)];
        end
    end

    // ---- stage 2: pick the bit -------------------------------------------------
    always_ff @(posedge clk) begin
        if (rst) begin
            best_lo_valid <= 1'b0;
            best_hi_valid <= 1'b0;
            best_lo       <= '0;
            best_hi       <= '0;
        end else begin
            best_lo_valid <= s1_any;
            best_hi_valid <= s1_any;
            best_lo       <= {s1_lo_w, lowest_b(s1_lo_word)};
            best_hi       <= {s1_hi_w, highest_b(s1_hi_word)};
        end
    end

endmodule

`default_nettype wire
