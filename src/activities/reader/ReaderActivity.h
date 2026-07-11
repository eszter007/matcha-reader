#pragma once
#include <memory>

#include "activities/Activity.h"
#include "activities/home/FileBrowserActivity.h"

class Epub;
class Xtc;
class Txt;
namespace manga { class MangaBook; }

class ReaderActivity final : public Activity {
  std::string initialBookPath;
  std::string currentBookPath;  // Track current book path for navigation
  // Non-static: needs renderer to release font caches before the heap-hungry Epub::load().
  std::unique_ptr<Epub> loadEpub(const std::string& path) const;
  static std::unique_ptr<Xtc> loadXtc(const std::string& path);
  static std::unique_ptr<Txt> loadTxt(const std::string& path);
  static std::unique_ptr<manga::MangaBook> loadManga(const std::string& path);
  static bool isXtcFile(const std::string& path);
  static bool isTxtFile(const std::string& path);
  static bool isBmpFile(const std::string& path);
  static bool isMangaFolder(const std::string& path);

  void goToLibrary(const std::string& fromBookPath = "");
  void onGoToEpubReader(std::unique_ptr<Epub> epub);
  void onGoToXtcReader(std::unique_ptr<Xtc> xtc);
  void onGoToTxtReader(std::unique_ptr<Txt> txt);
  void onGoToBmpViewer(const std::string& path);
  void onGoToMangaReader(std::unique_ptr<manga::MangaBook> manga);

  void onGoBack();

 public:
  explicit ReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialBookPath)
      : Activity("Reader", renderer, mappedInput), initialBookPath(std::move(initialBookPath)) {}
  void onEnter() override;
  bool isReaderActivity() const override { return true; }
};
