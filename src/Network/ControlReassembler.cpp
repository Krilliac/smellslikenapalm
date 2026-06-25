// src/Network/ControlReassembler.cpp
// See ControlReassembler.h.

#include "Network/ControlReassembler.h"

#include "Network/BitReader.h"
#include "Network/BitWriter.h"
#include "Network/ControlChannel.h"
#include "Network/NetMessages.h"

namespace PacketCodec {

namespace {

// Copy `count` bits from `in` (current position) into `out`, LSB-first.
void CopyBits(BitReader& in, BitWriter& out, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        out.WriteBit(in.ReadBit());
    }
}

} // namespace

ControlReassembler::ControlReassembler(MessageFn onMessage)
    : m_onMessage(std::move(onMessage)) {}

void ControlReassembler::OnBunch(const Bunch& bunch) {
    // Only reliable control-channel (index 0) bunches participate in the ordered
    // message stream. Everything else is ignored here.
    if (bunch.chIndex != static_cast<uint32_t>(kControlChannelIndex) || !bunch.bReliable) {
        return;
    }
    // Already-consumed sequence numbers are duplicate retransmits - ignore.
    if (bunch.chSequence < m_nextSeq) {
        return;
    }
    m_pending[bunch.chSequence] = bunch; // dedup: same seq overwrites identical data
    Drain();
}

void ControlReassembler::Drain() {
    // Append every in-order ready bunch's payload bits to the accumulator.
    auto it = m_pending.find(m_nextSeq);
    while (it != m_pending.end()) {
        const Bunch& b = it->second;

        BitWriter w;
        if (m_accumBits > 0) {
            BitReader existing(m_accumBytes.data(), m_accumBytes.size(), m_accumBits);
            CopyBits(existing, w, m_accumBits);
        }
        BitReader br(b.payload.data(), b.payload.size(), b.payloadBits);
        CopyBits(br, w, b.payloadBits);

        m_accumBytes = w.GetBytes();
        m_accumBits += b.payloadBits;

        m_pending.erase(it);
        ++m_nextSeq;
        it = m_pending.find(m_nextSeq);
    }

    PeelMessages();
}

void ControlReassembler::PeelMessages() {
    while (m_accumBits > 0) {
        BitReader r(m_accumBytes.data(), m_accumBytes.size(), m_accumBits);
        NMT type;
        if (!ControlChannel::ConsumeMessage(r, type)) {
            // Incomplete (need more bunches) or an NMT we can't delimit. Stop.
            break;
        }
        const size_t consumed = r.BitPos();
        if (consumed == 0 || consumed > m_accumBits) {
            break; // safety: never loop forever / over-read
        }

        // Extract the consumed bits as the byte-aligned message payload.
        BitReader src(m_accumBytes.data(), m_accumBytes.size(), m_accumBits);
        BitWriter msg;
        CopyBits(src, msg, consumed);
        if (m_onMessage) {
            m_onMessage(msg.GetBytes());
        }

        DropFrontBits(consumed);
    }
}

void ControlReassembler::DropFrontBits(size_t bitCount) {
    if (bitCount >= m_accumBits) {
        m_accumBytes.clear();
        m_accumBits = 0;
        return;
    }
    const size_t remaining = m_accumBits - bitCount;
    BitReader r(m_accumBytes.data(), m_accumBytes.size(), m_accumBits);
    for (size_t i = 0; i < bitCount; ++i) {
        r.ReadBit(); // skip consumed prefix
    }
    BitWriter w;
    CopyBits(r, w, remaining);
    m_accumBytes = w.GetBytes();
    m_accumBits = remaining;
}

} // namespace PacketCodec
