package p;
  typedef enum logic [1:0] {A, B=2, C} e_t;
  typedef struct packed { logic a; logic [3:0] b; } s_t;
  class c extends base;
    int x;
    function new(); super.new(); endfunction
    virtual task run(); endtask
  endclass
endpackage
