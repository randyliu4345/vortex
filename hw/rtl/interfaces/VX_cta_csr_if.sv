`include "VX_define.vh"

interface VX_cta_csr_if import VX_gpu_pkg::*; ();

    csr_cta_data_t data;
    logic valid;
    logic [`CLOG2(`NUM_WARPS)-1:0] wid;

    modport master (
        output data,
        output valid,
        output wid
    );

    modport slave (
        input data,
        input valid,
        input wid
    );

endinterface
