#include "videovault/app/batch_worker.hpp"

#include "videovault/core/vault.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <QApplication>

namespace videovault::app {
namespace {

// Shared across the parallel file workers of one batch.
struct BatchState {
    std::atomic<std::size_t> next{0};
    std::atomic<std::int64_t> bytes_done{0};
    std::atomic<int> files_done{0};
    std::atomic<bool> stop{false};
    std::mutex error_mutex;
    QString first_error;
};

constexpr int kMaxParallelFiles = 4;

int workerCount() {
    const auto cores = std::thread::hardware_concurrency();
    const auto count = cores == 0 ? 2 : static_cast<int>(cores);
    return std::clamp(count, 1, kMaxParallelFiles);
}

// Runs `work(index)` concurrently over [0, count); stops pulling new items
// once `state->stop` is set (first error). `fileFinished` is emitted from the
// finishing threads as each item completes.
template <typename Work>
void runParallel(const std::shared_ptr<BatchState>& state,
                 const std::size_t count,
                 BatchWorker* worker,
                 Work&& work) {
    const int threads = workerCount();
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(threads));
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            while (!state->stop.load()) {
                const std::size_t index = state->next.fetch_add(1);
                if (index >= count) {
                    break;
                }
                work(static_cast<int>(index));
                const int done = state->files_done.fetch_add(1) + 1;
                emit worker->fileFinished(done, static_cast<int>(count));
            }
        });
    }
    for (auto& thread : pool) {
        thread.join();
    }
}

} // namespace

BatchWorker::BatchWorker(QObject* parent)
    : QObject(parent) {}

void BatchWorker::importFiles(
    std::shared_ptr<std::vector<std::filesystem::path>> sources,
    std::shared_ptr<videovault::core::Vault> vault,
    const std::int64_t folder_id) {
    // Size pass first so the aggregate bar has a total (plaintext bytes).
    std::vector<std::int64_t> sizes;
    sizes.reserve(sources->size());
    std::int64_t total_bytes = 0;
    for (const auto& path : *sources) {
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        const auto safe = error ? 0 : static_cast<std::int64_t>(size);
        sizes.push_back(safe);
        total_bytes += safe;
    }

    const auto state = std::make_shared<BatchState>();
    std::atomic<int> imported{0};
    runParallel(state, sources->size(), this,
        [&](const int index) {
            const auto reported = std::make_shared<std::atomic<std::int64_t>>(0);
            const auto outcome = vault->import_file((*sources)[index],
                [this, state, reported, file_size = sizes[index], total_bytes](const double fraction) {
                    // The core reports a cumulative fraction. Adding that
                    // fraction on every tick counted the same bytes many
                    // times and ran the bar off the end of a large drop.
                    const auto clamped = std::clamp(fraction, 0.0, 1.0);
                    const auto target = static_cast<std::int64_t>(
                        clamped * static_cast<double>(file_size));
                    const auto previous = reported->exchange(target);
                    const auto delta = target - previous;
                    if (delta <= 0) {
                        return;
                    }
                    const auto done = state->bytes_done.fetch_add(delta) + delta;
                    emit progress(std::min(done, total_bytes), total_bytes);
                },
                folder_id);
            if (!outcome) {
                std::lock_guard<std::mutex> guard(state->error_mutex);
                if (state->first_error.isEmpty()) {
                    state->first_error = QString::fromUtf8(
                        videovault::core::user_message(outcome.error().code).data());
                    state->stop.store(true);
                }
            } else {
                ++imported;
            }
        });

    if (state->first_error.isEmpty()) {
        emit finished(true,
            QStringLiteral("Imported %1 video(s).").arg(imported.load()), imported.load());
    } else {
        emit finished(false, state->first_error, imported.load());
    }
}

void BatchWorker::restoreVideos(
    std::shared_ptr<std::vector<std::int64_t>> ids,
    std::shared_ptr<std::filesystem::path> target_dir,
    std::shared_ptr<videovault::core::Vault> vault) {
    // Weight each video by its plaintext size so the bar reflects real work.
    std::unordered_map<std::int64_t, std::int64_t> weights;
    std::int64_t total_bytes = 0;
    if (auto listed = vault->list_videos(); listed) {
        for (const auto& video : listed.value()) {
            weights.emplace(video.id, static_cast<std::int64_t>(video.original_size));
            total_bytes += static_cast<std::int64_t>(video.original_size);
        }
    }
    if (total_bytes <= 0) {
        total_bytes = static_cast<std::int64_t>(ids->size()) * 1000; // fallback
    }

    const auto state = std::make_shared<BatchState>();
    std::atomic<int> restored{0};
    runParallel(state, ids->size(), this,
        [&](const int index) {
            const auto video_id = (*ids)[index];
            const auto weight = weights.count(video_id) != 0U
                ? weights[video_id] : 1000;
            const auto reported = std::make_shared<std::atomic<std::int64_t>>(0);
            const auto outcome = vault->restore_video(video_id, *target_dir,
                [this, state, reported, weight, total_bytes](const double fraction) {
                    const auto clamped = std::clamp(fraction, 0.0, 1.0);
                    const auto target = static_cast<std::int64_t>(
                        clamped * static_cast<double>(weight));
                    const auto previous = reported->exchange(target);
                    const auto delta = target - previous;
                    if (delta <= 0) {
                        return;
                    }
                    const auto done = state->bytes_done.fetch_add(delta) + delta;
                    emit progress(std::min(done, total_bytes), total_bytes);
                });
            if (!outcome) {
                std::lock_guard<std::mutex> guard(state->error_mutex);
                if (state->first_error.isEmpty()) {
                    state->first_error = QString::fromUtf8(
                        videovault::core::user_message(outcome.error().code).data());
                    state->stop.store(true);
                }
            } else {
                ++restored;
            }
        });

    if (state->first_error.isEmpty()) {
        emit finished(true,
            QStringLiteral("Restored %1 video(s).").arg(restored.load()), restored.load());
    } else {
        emit finished(false, state->first_error, restored.load());
    }
}

void BatchWorker::removeVideos(
    std::shared_ptr<std::vector<std::int64_t>> ids,
    std::shared_ptr<videovault::core::Vault> vault) {
    const auto total = static_cast<qlonglong>(ids->size());
    const auto state = std::make_shared<BatchState>();
    std::atomic<int> removed{0};
    runParallel(state, ids->size(), this,
        [&](const int index) {
            const auto outcome = vault->remove_video((*ids)[index]);
            emit progress(static_cast<qlonglong>(state->files_done.load() + 1), total);
            if (!outcome) {
                std::lock_guard<std::mutex> guard(state->error_mutex);
                if (state->first_error.isEmpty()) {
                    state->first_error = QString::fromUtf8(
                        videovault::core::user_message(outcome.error().code).data());
                    state->stop.store(true);
                }
            } else {
                ++removed;
            }
        });

    if (state->first_error.isEmpty()) {
        emit finished(true,
            QStringLiteral("Removed %1 video(s).").arg(removed.load()), removed.load());
    } else {
        emit finished(false, state->first_error, removed.load());
    }
}

} // namespace videovault::app
