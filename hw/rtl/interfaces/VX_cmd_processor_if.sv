`include "VX_define.vh"

interface VX_cmd_processor_if();
    wire                    valid;
    wire                    ready;

    `UNUSED_VAR (ready);

    // assign ready = '1;

    modport master (
        output valid,
        input  ready
    );

    modport slave (
        input valid,
        output ready
    );

endinterface
