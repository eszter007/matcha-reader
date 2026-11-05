#include "ReaderSessionReporter.h"

#include <Arduino.h>
#include <KOReaderDocumentId.h>
#include <TrustedTime.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "util/PluginEvents.h"

void ReaderSessionReporter::noteTurn(const bool forward, const bool succeeded) {
  pendingTurn_.store(forward && succeeded ? TURN_FORWARD : TURN_OTHER, std::memory_order_release);
}

void ReaderSessionReporter::pageRendered(const int progressBp) {
  const uint8_t turn = pendingTurn_.exchange(TURN_NONE, std::memory_order_acq_rel);
  if (turn != TURN_NONE) session_.noteTurn(turn == TURN_FORWARD, turn == TURN_FORWARD);
  session_.onRenderComplete(millis(), trustedtime::trustedNow(), progressBp);
}

void ReaderSessionReporter::flush(const std::string& bookPath, const std::string& documentPath) {
  if (!session_.isEmitWorthy() || !pluginevents::anySubscriber(pluginevents::Event::ReaderSession)) {
    session_.reset();
    return;
  }

  const std::string document = KOReaderDocumentId::calculate(documentPath);
  const bool validDocument =
      document.size() == 32 && std::all_of(document.begin(), document.end(), [](const unsigned char c) {
        return std::isdigit(c) || (c >= 'a' && c <= 'f');
      });
  if (validDocument) {
    char startTime[24];
    char endTime[24];
    char duration[16];
    char startProgress[8];
    char endProgress[8];
    snprintf(startTime, sizeof(startTime), "%lld", static_cast<long long>(session_.startTime()));
    snprintf(endTime, sizeof(endTime), "%lld", static_cast<long long>(session_.endTime()));
    snprintf(duration, sizeof(duration), "%lu", static_cast<unsigned long>(session_.durationSeconds()));
    snprintf(startProgress, sizeof(startProgress), "%u", session_.startProgressBp());
    snprintf(endProgress, sizeof(endProgress), "%u", session_.endProgressBp());
    const pluginevents::Var vars[] = {{"book", bookPath.c_str()},       {"document", document.c_str()},
                                      {"start_time", startTime},        {"end_time", endTime},
                                      {"duration_seconds", duration},   {"start_progress_bp", startProgress},
                                      {"end_progress_bp", endProgress}, {"progress_scale", "10000"}};
    pluginevents::emit(pluginevents::Event::ReaderSession, vars, 8);
  }
  session_.reset();
}
