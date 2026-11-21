#include <ArduinoJson.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <KOReaderCredentialStore.h>
#include <KOReaderDocumentId.h>
#include <KOReaderSyncClient.h>
#include <TrustedTime.h>

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "sync/BookmarkSync.h"
#include "sync/SyncEngine.h"
#include "sync/SyncRequest.h"
#include "util/BookmarkFile.h"
#include "util/BookmarkUtil.h"

// The KOSync progress client and document ids are replaced by fakes: the engine's sequencing is under test.
std::vector<std::string> progressFetches;
bool primaryProgressMissing = false;
int KOReaderSyncClient::lastHttpCode = 0;
KOReaderSyncClient::Error KOReaderSyncClient::getProgress(const std::string& documentHash, KOReaderProgress& out) {
  progressFetches.push_back(documentHash);
  const bool primary = documentHash == "filename-id";
  if (primary && primaryProgressMissing) return NOT_FOUND;
  out.progress = primary ? "/primary" : "/alternate";
  return OK;
}
KOReaderSyncClient::Error KOReaderSyncClient::updateProgress(const KOReaderProgress&) { return OK; }
std::string KOReaderDocumentId::calculate(const std::string&) { return "binary-id"; }
std::string KOReaderDocumentId::calculateFromFilename(const std::string&) { return "filename-id"; }

namespace {
constexpr const char* book = "/book.epub";
const std::string document(32, 'a');
std::vector<std::string> uploads;

std::string hexId(const std::string& xpath) {
  char hex[BookmarkUtil::SYNC_ID_HEX_SIZE];
  BookmarkUtil::formatSyncId(BookmarkUtil::syncId(xpath), hex);
  return hex;
}

BookmarkEntry bookmark(const std::string& xpath, const bool synced) {
  BookmarkEntry entry;
  entry.xpath = xpath;
  entry.percentage = 0.5f;
  entry.summary = "summary";
  entry.synced = synced;
  return entry;
}

size_t acceptedCount(const std::string& body) {
  JsonDocument doc;
  deserializeJson(doc, body);
  return doc["items"].size();
}

void localDeletionIsRemembered() {
  fake::reset();
  // Never synced: nothing to tell a server.
  assert(BookmarkFile::save(book, {bookmark("/unsynced", false)}));
  assert(BookmarkFile::save(book, {}));
  BookmarkFile::SyncState none;
  std::vector<BookmarkEntry> empty;
  BookmarkFile::load(book, empty, &none);
  assert(none.deleted.empty());

  // A renamed bookmark is saved without its synced flag; deleting it must still reach the server.
  assert(BookmarkFile::saveSynced(book, {bookmark("/kept", true), bookmark("/removed", true)}, "reader@server"));
  assert(BookmarkFile::save(book, {bookmark("/kept", true), bookmark("/removed", false)}));
  assert(BookmarkFile::save(book, {bookmark("/kept", true)}));

  std::vector<BookmarkEntry> bookmarks;
  BookmarkFile::SyncState state;
  assert(BookmarkFile::load(book, bookmarks, &state));
  assert(bookmarks.size() == 1 && bookmarks[0].synced);
  assert(state.deleted.size() == 1 && state.deleted[0] == BookmarkUtil::syncId("/removed"));
}

void bookmarksMerge() {
  fake::reset();
  uploads.clear();
  const std::string account = SyncRequest::account() + "#" + document;
  BookmarkEntry renamed = bookmark("/renamed-there", true);
  renamed.name = "old name";
  assert(
      BookmarkFile::saveSynced(book, {bookmark("/gone-here", true), bookmark("/gone-there", true), renamed}, account));
  BookmarkEntry added = bookmark("/new-here", false);
  added.name = "mine";
  added.visibleTextOffset = 77;
  added.hasVisibleTextOffset = true;
  assert(BookmarkFile::save(book, {added, bookmark("/gone-there", true), renamed}));

  fake::server = [](const std::string& method, const std::string& url, const std::string& body) -> std::string {
    assert(url.find("/api/v1/bookmarks/" + document) != std::string::npos);
    if (method == "PUT") {
      uploads.push_back(body);
      return "{\"accepted\":" + std::to_string(acceptedCount(body)) + "}";
    }
    if (url.find("cursor=0") != std::string::npos) {
      return "{\"sync_version\":2,\"cursor\":5,\"more\":true,\"items\":[{\"id\":\"" + hexId("/new-there") +
             "\",\"xpath\":\"/new-there\",\"percentage\":0.25,\"summary\":\" far  away \",\"si\":3,\"pc\":9,\"pp\":2,"
             "\"name\":\"theirs\",\"vo\":1234,\"deleted\":0},{\"id\":\"" +
             hexId("/renamed-there") +
             "\",\"xpath\":\"/renamed-there\",\"percentage\":0.5,\"name\":\"new name\","
             "\"deleted\":0},{\"id\":\"" +
             hexId("/other") + "\",\"xpath\":\"/mismatch\",\"percentage\":0.1,\"deleted\":0}]}";
    }
    assert(url.find("cursor=5") != std::string::npos);
    return "{\"cursor\":9,\"more\":false,\"items\":[{\"id\":\"" + hexId("/gone-there") + "\",\"deleted\":1}]}";
  };

  SyncRequest sync;
  assert(sync.ready());
  assert(bookmarkSync::run(sync, book, document));

  // The local deletion is uploaded before the new bookmark.
  assert(uploads.size() == 2);
  assert(uploads[0] == "{\"items\":[{\"id\":\"" + hexId("/gone-here") + "\",\"deleted\":1}]}");
  assert(uploads[1].find("\"xpath\":\"/new-here\"") != std::string::npos);
  assert(uploads[1].find("\"name\":\"mine\",\"vo\":77") != std::string::npos);
  assert(uploads[1].find("/gone-there") == std::string::npos);

  std::vector<BookmarkEntry> bookmarks;
  BookmarkFile::SyncState state;
  assert(BookmarkFile::load(book, bookmarks, &state));
  assert(state.deleted.empty() && state.account == account);
  assert(bookmarks.size() == 3);
  assert(bookmarks[0].xpath == "/new-there" && bookmarks[0].synced && bookmarks[0].summary == "far away");
  assert(bookmarks[0].computedSpineIndex == 3 && bookmarks[0].computedChapterProgress == 2);
  assert(bookmarks[0].name == "theirs" && bookmarks[0].hasVisibleTextOffset && bookmarks[0].visibleTextOffset == 1234);
  assert(bookmarks[1].xpath == "/new-here" && bookmarks[1].synced && bookmarks[1].name == "mine");
  // A rename made on another device arrives; a server item without a name would leave it alone.
  assert(bookmarks[2].xpath == "/renamed-there" && bookmarks[2].name == "new name");

  // A second sync has nothing left to upload.
  uploads.clear();
  fake::server = [](const std::string& method, const std::string&, const std::string&) -> std::string {
    assert(method == "GET");
    return "{\"sync_version\":2,\"cursor\":0,\"more\":false,\"items\":[]}";
  };
  assert(bookmarkSync::run(sync, book, document));

  // Another account has none of these bookmarks, so all of them are uploaded again.
  syncCredentials.username = "other";
  fake::server = [](const std::string& method, const std::string&, const std::string& body) -> std::string {
    if (method == "GET") return "{\"sync_version\":2,\"cursor\":0,\"more\":false,\"items\":[]}";
    uploads.push_back(body);
    return "{\"accepted\":" + std::to_string(acceptedCount(body)) + "}";
  };
  assert(bookmarkSync::run(sync, book, document));
  assert(uploads.size() == 1 && acceptedCount(uploads[0]) == 3);
  syncCredentials.username = "reader";
}

void engineGatesEveryUpload() {
  fake::reset();
  assert(BookmarkFile::save(book, {bookmark("/local", false)}));
  syncCredentials.enabled = false;  // Sync Clippings off
  progressFetches.clear();

  // A server without the extensions is probed, receives nothing else, and still syncs progress.
  int requests = 0;
  fake::server = [&](const std::string& method, const std::string&, const std::string&) -> std::string {
    assert(method == "GET");
    ++requests;
    return "{\"items\":[]}";
  };
  syncEngine::Pull pull = syncEngine::pull(book, false);
  assert(requests == 1 && pull.extensionsSynced);
  assert(pull.documentHash == "filename-id" && pull.result == KOReaderSyncClient::OK);
  assert(pull.remote.progress == "/primary" && !pull.alternate);

  // A plain KOSync server gets the standard progress request and no extension request at all.
  syncCredentials.serverType = KOReaderServerType::KOSYNC;
  progressFetches.clear();
  pull = syncEngine::pull(book, false);
  assert(requests == 1);
  assert(progressFetches == std::vector<std::string>{"filename-id"} && pull.remote.progress == "/primary");
  syncCredentials.serverType = KOReaderServerType::CROSSPOINT;

  // Smart sync adopts the record stored under the other document id when the primary has none.
  primaryProgressMissing = true;
  pull = syncEngine::pull(book, true);
  assert(pull.result == KOReaderSyncClient::OK && pull.remote.progress == "/alternate" && !pull.alternate);
  primaryProgressMissing = false;
  pull = syncEngine::pull(book, true);
  assert(pull.remote.progress == "/primary" && pull.alternate && pull.alternate->progress == "/alternate");

  // A supporting server receives the bookmark.
  uploads.clear();
  fake::server = [](const std::string& method, const std::string& url, const std::string& body) -> std::string {
    if (method == "GET") return "{\"sync_version\":2,\"cursor\":0,\"more\":false,\"items\":[]}";
    uploads.push_back(url);
    return "{\"accepted\":" + std::to_string(acceptedCount(body)) + "}";
  };
  assert(syncEngine::pull(book, false).extensionsSynced);
  assert(uploads.size() == 1);
  assert(uploads[0].find("/api/v1/bookmarks/") != std::string::npos);
  syncCredentials.enabled = true;
}

void summaryIsCutOnCharacterBoundary() {
  // 71 ASCII bytes and a two-byte character: a cut at 72 bytes would split it.
  assert(BookmarkUtil::sanitizeBookmarkSummary(std::string(71, 'a') + "\xC3\xA9").size() == 71);
  assert(BookmarkUtil::sanitizeBookmarkSummary(std::string(70, 'a') + "\xC3\xA9").size() == 72);
}
}  // namespace

int main() {
  fake::syncHeap = 100000;  // room for the heap transfer buffer and TLS
  localDeletionIsRemembered();
  bookmarksMerge();
  engineGatesEveryUpload();
  summaryIsCutOnCharacterBoundary();
  puts("Sync extension tests passed");
}
