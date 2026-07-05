// Sample SV netlist for (eda:load-sv) Issue #616 test fixture.
// Each non-empty line is a colon-separated directive the parser
// understands (interface / modport / constraint / property /
// coverpoint). Layout matches the line-based parser in
// evaluator_primitives_eda.cpp.

interface : iface_a
modport : mp_a : p_in p_out
constraint : c_a : a > 0
property : p_a : req |=> ack
coverpoint : v_a : 0 1 2 3
