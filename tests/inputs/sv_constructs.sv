// Constructs from handoff item 5: clocking, covergroup bins, assertions and
// property bodies, DPI, bind, constraints, randomize-with.
module m;
clocking cb @(posedge clk);
default input #1step output #2;
input a, b;
output c;
endclocking
covergroup cg @(posedge clk);
cp: coverpoint x {
bins low = {[0:3]};
bins mid[] = {[4:7]};
illegal_bins bad = {8};
}
endgroup
property p_req(req, ack);
@(posedge clk) disable iff (rst)
req |-> ##[1:3] ack;
endproperty
a_req: assert property (p_req(req, ack)) else $error("no ack");
import "DPI-C" function int c_add(input int a, input int b);
export "DPI-C" function sv_add;
endmodule
bind dut checker_m u_chk (.clk(clk), .a(a));
class k;
rand int x, y;
constraint c1 {x inside {[0:10]}; y > x; solve x before y; if (x > 5) y < 20; else y < 10;}
constraint c2 {foreach (arr[i]) arr[i] < 10; x dist {0 := 1, [1:5] :/ 4};}
function void f(); void'(randomize(x) with {x < 3;}); endfunction
endclass

module labeled_assertions;
  a_req: assert property (p_req(req, ack)) else $error("no ack");
  assert property (p_req(req, ack)) else $error("no ack");
  c_req: cover property (p_req(req, ack));
  initial begin
    a1: assert (x == 1) else $error("bad");
    assert (x == 1) else $error("bad");
  end
endmodule

module long_assertions;
  a_req_with_long_name: assert property (@(posedge clk) disable iff (rst) req_signal |-> ##[1:3] ack_signal) else $error("no ack for request %0d", id);
  initial begin
    assert (x == 1) else begin
      $error("bad");
      errors++;
    end
    assert (x == 1) $display("ok"); else $error("bad");
    if (a) assert (x); else y = 1;
  end
endmodule

class constraint_conditionals;
  constraint c1 {
    if (x > 5) y < 20; else y < 10;
    if (mode == A) {
      y < 3;
      z > 1;
    } else if (mode == B) {
      y < 4;
    } else {
      y < 5;
    }
    foreach (arr[i]) arr[i] < 10;
    foreach (arr[i]) {
      arr[i] < 10;
      arr[i] > 0;
    }
    x -> y == 1;
  }
endclass

module more_constructs;
default clocking cb @(posedge clk);
default input #1step output negedge;
input #2 a;
output #1 b, c;
inout d;
endclocking
global clocking gclk @(posedge sys_clk); endclocking
sequence s_hs(a, b, int n = 2);
@(posedge clk) a ##[1:n] b;
endsequence
property p_case(m);
@(posedge clk) case (m)
1: a |=> b;
default: 1;
endcase
endproperty
property p_if;
@(posedge clk) if (mode) a |-> b else c |-> d;
endproperty
property p_not; not (a ##1 b); endproperty
assume property (@(posedge clk) a |-> b);
initial expect (@(posedge clk) a ##1 b) else $error("e");
import "DPI-C" context task c_wait(input int n);
import "DPI-C" pure function real c_sin(input real x);
export "DPI-C" task sv_task;
endmodule
bind top.u_dut checker_m #(.W(8)) u_chk (.*);
class more_constraints;
rand bit [3:0] a, b;
constraint c3 {solve a before b; a dist {[0:3] := 1, [4:15] :/ 2}; unique {a, b};}
constraint c4 {soft a == 1; b inside {1, 2, 3};}
endclass

// a modport port group stays on one line when it fits, else one port per line
interface modports;
  modport MulDivUnit(
    input clk, rst, stall, dataInA, dataInB, mulGetUpper, mulCode, divCode, divReq, divAcquire, divRelease,
    output mulDataOut, divDataOut
  );
  modport Scheduler(input divFree, output x);
endinterface
