interface i #(
    parameter AUSER_WIDTH = `AUSER_DEFAULT_WIDTH, ///< Width of the auser signal. RI5CY, Ibex, CV32E40* do not have the auser signal.
    parameter WUSER_WIDTH = `WUSER_DEFAULT_WIDTH  ///< Width
) ();
endinterface
