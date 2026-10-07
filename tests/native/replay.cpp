#include "SerialTransport.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
  assert(argc == 2);
  unsigned ack = std::strtoul(argv[1], nullptr, 10);
  SerialTransport transport;
  transport.begin(77);
  assert(transport.requestProtocolV2());
  transport.serviceOutput();
  for (unsigned i = 0; i < 64; ++i) {
    assert(transport.enqueueText("event", 1, SerialTransport::PRIORITY_SAFETY, 1, 0));
  }
  for (unsigned i = 0; i < 1000; ++i) transport.serviceOutput();
  assert(transport.highestTransmittedSequence() == 64);
  Serial.output.clear();
  assert(transport.requestResend(77, 1) == SerialTransport::RESEND_ACCEPTED);
  transport.serviceOutput();  // Only 32 bytes: replay of sequence 1 is in flight.
  assert(transport.acknowledge(77, ack));
  for (unsigned i = 0; i < ack; ++i) {
    assert(transport.enqueueText("new event", 1, SerialTransport::PRIORITY_SAFETY, 1, 0));
  }
  Serial.capacity = 1;
  for (unsigned i = 0; i < 10000; ++i) transport.serviceOutput();
  assert(transport.diagnostics().queueUsed == 64);
  assert(!transport.overflowLatched());
  std::fwrite(Serial.output.data(), 1, Serial.output.size(), stdout);
}
