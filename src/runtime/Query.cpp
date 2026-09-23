#include "qobs/runtime/Observatory.hpp"

namespace qobs {

Status Observatory::inspect(const InspectQuery& query, InspectResult& result) const {
  return store_->inspect(query, result);
}

Status Observatory::history(const HistoryQuery& query, HistoryResult& result) const {
  return store_->history(query, result);
}

Status Observatory::pressure(const PressureQuery& query, PressureResult& result) const {
  return store_->pressure(query, result);
}

Status Observatory::explain(const ExplainQuery& query, ExplainResult& result) const {
  return store_->explain(query, result);
}

Status Observatory::export_data(const ExportQuery& query, ExportResult& result) const {
  return store_->export_data(query, result);
}

Status Observatory::events(const EventQuery& query, EventResult& result) const {
  return store_->events(query, result);
}

Status Observatory::contention(const ContentionQuery& query, ContentionResult& result) const {
  return store_->contention(query, result);
}

Status Observatory::microburst(const MicroburstQuery& query, MicroburstResult& result) const {
  return store_->microburst(query, result);
}

Status Observatory::sources(std::vector<SourceRecord>& result) const {
  return store_->sources(result);
}

Status Observatory::conflicts(std::vector<ConflictRecord>& result) const {
  return store_->conflicts(result);
}

}  // namespace qobs
