// netsim.h
//
// Library interface for sending frames to netsim.
// Students include this header and link with netsim_lib.cc.
//
// Compile example:
//   g++ -O2 -o sender sender.cc netsim_lib.cc

#ifndef NETSIM_H
#define NETSIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Return values for send_frame()
#define NETSIM_ACK    0   // Frame received correctly (CRC OK)
#define NETSIM_NAK    1   // Frame received with errors (CRC failed)
#define NETSIM_ERROR -1   // Communication error with netsim

// Send one frame to netsim and wait for the ACK/NAK response.
//
// Parameters:
//   frame      - pointer to the entire frame bytes:
//                  [size: 2 bytes BE] [payload: P bytes] [CRC: 4 bytes BE]
//   frame_size - total frame size in bytes (= 2 + payload_size + 4)
//
// Returns:
//   NETSIM_ACK    - frame was received correctly; advance to next data
//   NETSIM_NAK    - frame was corrupted in transit; resend the same frame
//   NETSIM_ERROR  - unable to communicate with netsim (treat as fatal)
//
// Notes:
//   - This function blocks until netsim responds.
//   - The student MUST construct the entire frame correctly before calling.
//   - When all data has been delivered (last frame received ACK), the sender
//     should simply return from main(); netsim detects EOF and exits.
int send_frame(const uint8_t *frame, int frame_size);

#ifdef __cplusplus
}
#endif

#endif // NETSIM_H
