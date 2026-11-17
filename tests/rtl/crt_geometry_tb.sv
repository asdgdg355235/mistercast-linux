// Test stimulus for the pinned, separately supplied MiSTer-CRT-Adjust RTL.
module crt_cabinet_tb;
    reg clk = 0;
    always #5 clk = ~clk;
    integer cycle = 0, x = 0, y = 0, frame = 0;
    integer source_height, size, tight, total, top;
    reg signed [5:0] vsize;
    initial begin
        if (!$value$plusargs("n=%d", source_height)) $fatal(1, "missing height");
        if (!$value$plusargs("s=%d", size)) $fatal(1, "missing size");
        if (!$value$plusargs("tight=%d", tight)) tight = 0;
        top = 20;
        total = source_height + (tight ? 32 : 48);
        vsize = -3 * size;
    end
    wire ce = cycle % 8 == 0;
    wire hs = x < 2;
    wire vs = y < 3;
    wire vb = y < top || y >= top + source_height;
    wire de = !vb && x >= 6 && x < 14;
    wire [7:0] source_y = y - top;
    wire [7:0] source_x = x - 6;
    wire [7:0] r, g, b;
    wire hs_out, vs_out, de_out, vb_out, ce_out;
    crt_vsize #(.LINE_PX(16)) dut (
        .clk(clk), .pxl_cen(ce), .active(1'b1), .tube_mode(1'b1), .vsize(vsize),
        .r_in(source_y), .g_in(source_x), .b_in(source_x ^ source_y),
        .hs_in(hs), .vs_in(vs), .de_in(de), .vb_in(vb),
        .r_out(r), .g_out(g), .b_out(b),
        .hs_out(hs_out), .vs_out(vs_out), .de_out(de_out), .vb_out(vb_out), .ce_out(ce_out)
    );

    always @(negedge clk) begin
        if (ce) begin
            x = x + 1;
            if (x == 24) begin
                x = 0;
                y = y + 1;
                if (y == total) begin
                    y = 0;
                    frame = frame + 1;

                end
            end
        end
        if (frame == 28 && y == top) $finish;
        cycle = cycle + 1;
    end

    wire trace_cabinet = frame == 27 || (frame == 28 && y < top);
    always @(posedge clk) begin
        if (trace_cabinet && ce && x == 5 && dut.t_active) begin
            $display("V %0d %0d %0d %0d %0d %0d %0d %0d %0d",
                dut.a_nat, $signed(dut.vsize_eff), dut.a_out_t, dut.step_q8,
                dut.t_row, dut.t_src, dut.alpha_eff, dut.nl - dut.nl_top, dut.t_vsdel);
        end
        if (trace_cabinet && ce_out && de_out) $display("RGB %0d %0d %0d", r, g, b);
    end
endmodule

module crt_horizontal_tb;
    reg clk_sys = 0;
    always #5 clk_sys = ~clk_sys;
    integer cycle = 0, x = 0, y = 0, frame = 0, rise_cycle = 0;
    integer size, shift, start_pixel, vertical_shift;
    reg signed [4:0] hsize_s;
    reg signed [8:0] hoffset;
    reg signed [5:0] voffset;
    initial begin
        if (!$value$plusargs("h=%d", size)) $fatal(1, "missing size");
        if (!$value$plusargs("x=%d", shift)) $fatal(1, "missing shift");
        if (!$value$plusargs("start=%d", start_pixel)) $fatal(1, "missing active start");
        if (!$value$plusargs("v=%d", vertical_shift)) $fatal(1, "missing vertical shift");
        hsize_s = size;
        hoffset = shift;
        voffset = vertical_shift;
    end
    localparam WIDTH = 32, TOTAL = 136, HEIGHT = 12, VTOTAL = 22;
    wire ce_pix = cycle % 16 == 0;
    wire crt_on = 1'b1;
    wire hs = x < 4;
    wire vs = y < 2;
    wire vb = y < 4 || y >= 4 + HEIGHT;
    wire hb = x < start_pixel || x >= start_pixel + WIDTH;
    wire [7:0] r = x - start_pixel, g = y - 4;

    // Copied at test time from the pinned example by check_reference.py.
    `include "read_ce.vh"

    wire [23:0] rgb, sys_rgb;
    wire hs_out, vs_out, hb_out, vb_out;
    wire sys_hs, sys_vs, sys_hb, sys_vb, sys_ref;
    crt_adjust #(.VTOTAL(VTOTAL), .HTOTAL(TOTAL), .HPOS_MODE(`POSITION_MODE)) dut (
        .clk(clk_sys), .pxl_cen(ce_pix), .pxl2_cen(rd_ce), .active(crt_on),
        .hsize(hsize_s), .hoffset(hoffset), .voffset(voffset),
        .r_in(r), .g_in(g), .b_in(r ^ g), .hs_in(hs), .vs_in(vs), .hb_in(hb), .vb_in(vb),
        .r_out(rgb[23:16]), .g_out(rgb[15:8]), .b_out(rgb[7:0]),
        .hs_out(hs_out), .vs_out(vs_out), .hb_out(hb_out), .vb_out(vb_out), .hs_ref_out(hs_ref)
    );
    crt_adjust_sys #(.VTOTAL(VTOTAL), .HTOTAL(TOTAL), .HPOS_MODE(`POSITION_MODE)) system_dut (
        .clk(clk_sys), .pxl_cen(ce_pix), .pxl2_cen(rd_ce), .active(crt_on),
        .hsize(hsize_s), .hoffset(hoffset), .voffset(voffset),
        .r_in(r), .g_in(g), .b_in(r ^ g), .hs_in(hs), .vs_in(vs), .hb_in(hb), .vb_in(vb),
        .r_out(sys_rgb[23:16]), .g_out(sys_rgb[15:8]), .b_out(sys_rgb[7:0]),
        .hs_out(sys_hs), .vs_out(sys_vs), .hb_out(sys_hb), .vb_out(sys_vb), .hs_ref_out(sys_ref)
    );

    always @(negedge clk_sys) begin
        if (ce_pix) begin
            x = x + 1;
            if (x == TOTAL) begin
                x = 0;
                y = y + 1;
                if (y == VTOTAL) begin
                    y = 0;
                    frame = frame + 1;
                    if (frame == 3) $finish;
                end
            end
        end
        cycle = cycle + 1;
    end

    always @(posedge clk_sys) begin
        if (hs_ref_rise) rise_cycle = cycle;
        if (frame == 2 && y == 11 && rd_ce && !hs_ref_rise)
            $display("H %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d",
                cycle - rise_cycle, dut.rdcnt, dut.rd_addr, dut.hb1, dut.hb0,
                dut.vb_active, hb_out, hs_out, vs_out, rgb[23:16], rgb[15:8]);
        if (frame == 2 && {rgb, hs_out, vs_out, hb_out, vb_out, hs_ref} !==
            {sys_rgb, sys_hs, sys_vs, sys_hb, sys_vb, sys_ref})
            $fatal(1, "core and sys variants diverge");
    end
endmodule
