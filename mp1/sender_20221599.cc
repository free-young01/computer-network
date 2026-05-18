#include "netsim.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

namespace {

constexpr size_t kMaxPayloadSize = 65535;
constexpr size_t kMaxFrameSize = 2 + kMaxPayloadSize + 4;
constexpr size_t kInitialPayloadSize = 64;
constexpr size_t kMinPayloadSize = 32;
constexpr size_t kAdaptiveMaxPayload = 32768;
constexpr bool kEnableAppendZeroFlush = false;
constexpr uint32_t kCrc32Polynomial = 0x04C11DB7u;

uint32_t crc32_mod2_division(const uint8_t *bytes, size_t length) {
    uint32_t remainder = 0;

    for (size_t i = 0; i < length; ++i) {
        remainder ^= static_cast<uint32_t>(bytes[i]) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            if (remainder & 0x80000000u) {
                remainder = (remainder << 1) ^ kCrc32Polynomial;
            } else {
                remainder <<= 1;
            }
        }
    }

    if constexpr (kEnableAppendZeroFlush) {
        for (int byte = 0; byte < 4; ++byte) {
            for (int bit = 0; bit < 8; ++bit) {
                if (remainder & 0x80000000u) {
                    remainder = (remainder << 1) ^ kCrc32Polynomial;
                } else {
                    remainder <<= 1;
                }
            }
        }
    }

    return remainder;
}

int build_frame(uint8_t *frame,
                const std::vector<uint8_t> &data,
                size_t offset,
                size_t payload_size) {
    frame[0] = static_cast<uint8_t>((payload_size >> 8) & 0xff);
    frame[1] = static_cast<uint8_t>(payload_size & 0xff);

    std::copy(data.begin() + static_cast<std::ptrdiff_t>(offset),
              data.begin() + static_cast<std::ptrdiff_t>(offset + payload_size),
              frame + 2);

    uint32_t crc = crc32_mod2_division(frame, 2 + payload_size);
    frame[2 + payload_size] = static_cast<uint8_t>((crc >> 24) & 0xff);
    frame[3 + payload_size] = static_cast<uint8_t>((crc >> 16) & 0xff);
    frame[4 + payload_size] = static_cast<uint8_t>((crc >> 8) & 0xff);
    frame[5 + payload_size] = static_cast<uint8_t>(crc & 0xff);

    return static_cast<int>(2 + payload_size + 4);
}

size_t next_payload_after_ack(size_t current_payload, unsigned consecutive_acks) {
    if (current_payload < 1024) {
        return std::min(current_payload * 2, kAdaptiveMaxPayload);
    }

    if (consecutive_acks >= 4) {
        size_t increase = std::max<size_t>(256, current_payload / 4);
        return std::min(current_payload + increase, kAdaptiveMaxPayload);
    }

    return current_payload;
}

size_t next_payload_after_nak(size_t current_payload) {
    if (current_payload <= kMinPayloadSize) {
        return kMinPayloadSize;
    }

    return std::max(current_payload / 4, kMinPayloadSize);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " input_file\n";
        return 1;
    }

    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
        std::cerr << "failed to open input file: " << argv[1] << "\n";
        return 1;
    }

    std::vector<uint8_t> data(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());

    if (!input.eof() && input.fail()) {
        std::cerr << "failed to read input file: " << argv[1] << "\n";
        return 1;
    }

    uint8_t frame[kMaxFrameSize];
    size_t offset = 0;
    size_t payload_size = kInitialPayloadSize;
    unsigned consecutive_acks = 0;

    while (offset < data.size()) {
        size_t remaining = data.size() - offset;
        size_t attempt_payload = std::min(payload_size, remaining);
        attempt_payload = std::min(attempt_payload, kMaxPayloadSize);

        int frame_size = build_frame(frame, data, offset, attempt_payload);
        int result = send_frame(frame, frame_size);

        if (result == NETSIM_ACK) {
            offset += attempt_payload;
            ++consecutive_acks;
            payload_size = next_payload_after_ack(payload_size, consecutive_acks);
            if (consecutive_acks >= 4) {
                consecutive_acks = 0;
            }
        } else if (result == NETSIM_NAK) {
            consecutive_acks = 0;
            payload_size = next_payload_after_nak(attempt_payload);
        } else {
            std::cerr << "send_frame failed\n";
            return 1;
        }
    }

    return 0;
}
