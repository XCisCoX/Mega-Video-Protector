#pragma once

#include <QObject>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace videovault::core {
class Vault;
} // namespace videovault::core

namespace videovault::app {

// Runs batch operations (multi-file import, multi-video restore) off the UI
// thread with up to four files processed in parallel, and reports progress to
// the UI thread via queued signals — the UI event loop is never blocked.
//
// `progress` carries a smooth aggregate byte count (monotonic, reported per
// chunk inside each file, so the bar moves continuously even for one large
// file); `fileFinished` reports "n of N files" for the status text. The slots
// take shared_ptr payloads so nothing is copied across threads; the vault is
// held alive for the duration of the batch.
class BatchWorker final : public QObject {
    Q_OBJECT
public:
    explicit BatchWorker(QObject* parent = nullptr);

public slots:
    void importFiles(
        std::shared_ptr<std::vector<std::filesystem::path>> sources,
        std::shared_ptr<videovault::core::Vault> vault);

    void restoreVideos(
        std::shared_ptr<std::vector<std::int64_t>> ids,
        std::shared_ptr<std::filesystem::path> target_dir,
        std::shared_ptr<videovault::core::Vault> vault);

    void removeVideos(
        std::shared_ptr<std::vector<std::int64_t>> ids,
        std::shared_ptr<videovault::core::Vault> vault);

signals:
    // Aggregate bytes processed / total bytes (qlonglong: imports can exceed
    // 2 GB). Monotonic and delivered per chunk, so the bar moves smoothly.
    void progress(qlonglong done_bytes, qlonglong total_bytes);

    // One more file/video finished.
    void fileFinished(int done_files, int total_files);

    // Emitted once at the end: ok=true with the success count and a summary,
    // or ok=false with the first error message and the partial count.
    void finished(bool ok, QString message, int count);
};

} // namespace videovault::app
