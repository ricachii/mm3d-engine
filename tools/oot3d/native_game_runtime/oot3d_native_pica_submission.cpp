#include "oot3d_native_title_profile.h"
#include "oot3d_native_pica_submission.h"
#include "oot3d_cpu_phase_probe.h"
#include "oot3d_native_capture_clock.h"
#include "oot3d_native_pica_transfer.h"

#include <algorithm>
#include <cstdlib>
#include <bit>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

#include <nlohmann/json.hpp>

namespace Oot3dNativeGame {
namespace {

using RuntimeProfileClock = std::chrono::steady_clock;
constexpr size_t kGeometrySnapshotCacheLimit = 4096U;
constexpr size_t kMaximumTextureSnapshotCacheEntries = 65536U;
constexpr size_t kMaximumSnapshotResourceBytes = 64U << 20U;
// Bound the persisted raw-copy ownership ledger. Producers accumulate only
// until a newer copy takes their bytes back, so the live count tracks distinct
// copy destinations rather than elapsed frames.
constexpr size_t kMaximumRawCopyProducers = 1024U;
constexpr size_t kMaximumRawCopySpansPerProducer = 4096U;

uint64_t RuntimeProfileElapsedNanoseconds(
    RuntimeProfileClock::time_point start) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            RuntimeProfileClock::now() - start).count());
}

void SetError(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

bool RangeFits(uint32_t baseAddress, size_t size) {
    return size != 0U &&
           static_cast<uint64_t>(baseAddress) + size <=
               static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) +
                   1U;
}

uint64_t HashSnapshotBytes(std::span<const uint8_t> bytes) {
    return HashOot3dPicaSnapshot(bytes);
}

// FNV-1a continued from `hash`: HashOot3dPicaSnapshot(a ++ b) equals
// ContinueSnapshotHash(HashOot3dPicaSnapshot(a), b).
uint64_t ContinueSnapshotHash(uint64_t hash, std::span<const uint8_t> bytes) {
    for (const auto value : bytes) hash = (hash ^ value) * 1099511628211ULL;
    return hash;
}

void HashCombine(size_t& hash, uint64_t value) {
    hash ^= static_cast<size_t>(
                value + 0x9e3779b97f4a7c15ULL +
                (static_cast<uint64_t>(hash) << 6U) +
                (static_cast<uint64_t>(hash) >> 2U));
}

} // namespace

Oot3dPicaPhysicalMemoryView::Oot3dPicaPhysicalMemoryView(
    std::vector<Oot3dPicaPhysicalMemoryRegion> regions, ReadView readView,
    WriteGeneration writeGeneration, MappingCheck mappingCheck)
    : mRegions(std::move(regions)), mReadView(std::move(readView)),
      mWriteGeneration(std::move(writeGeneration)),
      mMappingCheck(std::move(mappingCheck)) {
    if (!mReadView) {
        throw std::invalid_argument("PICA memory view requires a read callback");
    }
}

std::optional<uint32_t> Oot3dPicaPhysicalMemoryView::Translate(
    uint32_t physicalAddress, size_t size) const {
    if (!RangeFits(physicalAddress, size)) {
        return std::nullopt;
    }
    for (const auto& region : mRegions) {
        if (!RangeFits(region.PhysicalBaseAddress, region.Size) ||
            !RangeFits(region.GuestBaseAddress, region.Size) ||
            physicalAddress < region.PhysicalBaseAddress) {
            continue;
        }
        const uint64_t offset =
            static_cast<uint64_t>(physicalAddress) -
            region.PhysicalBaseAddress;
        if (offset + size <= region.Size) {
            const uint32_t guestAddress =
                region.GuestBaseAddress + static_cast<uint32_t>(offset);
            if (mMappingCheck ? mMappingCheck(guestAddress, size)
                              : mReadView(guestAddress, size).size() == size) {
                return guestAddress;
            }
        }
    }
    return std::nullopt;
}

std::optional<uint32_t> Oot3dPicaPhysicalMemoryView::TranslateGuest(
    uint32_t guestAddress, size_t size) const {
    if (!RangeFits(guestAddress, size)) {
        return std::nullopt;
    }
    for (const auto& region : mRegions) {
        if (!RangeFits(region.PhysicalBaseAddress, region.Size) ||
            !RangeFits(region.GuestBaseAddress, region.Size) ||
            guestAddress < region.GuestBaseAddress) {
            continue;
        }
        const uint64_t offset =
            static_cast<uint64_t>(guestAddress) - region.GuestBaseAddress;
        if (offset + size <= region.Size) {
            return region.PhysicalBaseAddress + static_cast<uint32_t>(offset);
        }
    }
    return std::nullopt;
}

bool Oot3dPicaPhysicalMemoryView::ForEachGuestAlias(
    uint32_t physicalAddress, size_t size, const GuestAliasVisitor& visitor) const {
    bool found = false;
    if (!RangeFits(physicalAddress, size)) return false;
    for (const auto& region : mRegions) {
        if (!RangeFits(region.PhysicalBaseAddress, region.Size) ||
            !RangeFits(region.GuestBaseAddress, region.Size) ||
            physicalAddress < region.PhysicalBaseAddress) {
            continue;
        }
        const uint64_t offset =
            static_cast<uint64_t>(physicalAddress) -
            region.PhysicalBaseAddress;
        if (offset + size > region.Size) {
            continue;
        }
        const uint32_t guestAddress =
            region.GuestBaseAddress + static_cast<uint32_t>(offset);
        if (mMappingCheck ? mMappingCheck(guestAddress, size)
                          : mReadView(guestAddress, size).size() == size) {
            found = true;
            if (!visitor(guestAddress, size)) return false;
        }
    }
    return found;
}

std::optional<std::pair<uint32_t, size_t>>
Oot3dPicaPhysicalMemoryView::GuestSegment(uint32_t guestAddress,
                                          size_t size) const {
    if (size == 0U) {
        return std::nullopt;
    }
    for (const auto& region : mRegions) {
        if (!RangeFits(region.GuestBaseAddress, region.Size) ||
            guestAddress < region.GuestBaseAddress) {
            continue;
        }
        const uint64_t offset =
            static_cast<uint64_t>(guestAddress) - region.GuestBaseAddress;
        if (offset >= region.Size) {
            continue;
        }
        const size_t available = region.Size - static_cast<size_t>(offset);
        return std::pair{guestAddress, std::min(size, available)};
    }
    return std::nullopt;
}

std::optional<std::span<const uint8_t>> Oot3dPicaPhysicalMemoryView::View(
    uint32_t physicalAddress, size_t size) const {
    const auto guestAddress = Translate(physicalAddress, size);
    if (!guestAddress.has_value()) {
        return std::nullopt;
    }
    const auto bytes = mReadView(*guestAddress, size);
    if (bytes.size() != size) {
        return std::nullopt;
    }
    return bytes;
}
std::optional<uint64_t>
Oot3dPicaPhysicalMemoryView::RangeWriteGeneration(uint32_t physicalAddress,
                                                  size_t size) const {
  if (!mWriteGeneration) return std::nullopt;
  std::optional<uint64_t> generation;
  const bool certified = ForEachGuestAlias(physicalAddress, size,
      [&](uint32_t guest, size_t bytes) {
          const auto alias = mWriteGeneration(guest, bytes);
          if (!alias || !*alias || (generation && *generation != *alias)) return false;
          generation = alias;
          return true;
      });
  // Providers may use independent opaque tokens for aliases. Unequal tokens
  // cannot be reduced with max/hash into a collision-free certificate.
  return certified ? generation : std::nullopt;
}

bool Oot3dPicaPhysicalMemoryView::Read(
    uint32_t physicalAddress, std::span<uint8_t> output) const {
    if (output.empty()) {
        return true;
    }
    const auto guestAddress = Translate(physicalAddress, output.size());
    const auto source = guestAddress.has_value()
                            ? mReadView(*guestAddress, output.size())
                            : std::span<const uint8_t>{};
    if (source.size() != output.size()) return false;
    std::copy(source.begin(), source.end(), output.begin());
    return true;
}

bool Oot3dPicaPhysicalMemoryView::ReadGuest(
    uint32_t guestAddress, std::span<uint8_t> output) const {
    if (output.empty()) return true;
    const auto source = mReadView(guestAddress, output.size());
    if (source.size() != output.size()) return false;
    std::copy(source.begin(), source.end(), output.begin());
    return true;
}

Oot3dNativePicaSubmissionQueue::Oot3dNativePicaSubmissionQueue(
    Oot3dPicaPhysicalMemoryView memory,
    bool deferGpuBackedDisplayTransfers)
    : mMemory(std::move(memory)),
      mDeferGpuBackedDisplayTransfers(deferGpuBackedDisplayTransfers) {}

void Oot3dNativePicaSubmissionQueue::SetRuntimeProfilingEnabled(
    bool enabled) {
    std::lock_guard guard(mQueueMutex);
    mRuntimeProfilingEnabled = enabled;
}

void Oot3dNativePicaSubmissionQueue::SetTexturePayloadTransform(
    TexturePayloadTransform transform) {
    std::lock_guard guard(mQueueMutex);
    mTexturePayloadTransform = std::move(transform);
    mTextureSnapshotCache.clear();
    ClearTextureFront();
}

Oot3dPicaSubmissionRuntimeProfile
Oot3dNativePicaSubmissionQueue::RuntimeProfile() const noexcept {
    std::lock_guard guard(mQueueMutex);
    return mRuntimeProfile;
}

size_t Oot3dNativePicaSubmissionQueue::TextureSnapshotKeyHash::operator()(
    const TextureSnapshotKey& key) const noexcept {
    size_t hash = 0;
    HashCombine(hash, key.PhysicalAddress);
    HashCombine(hash, key.Width);
    HashCombine(hash, key.Height);
    HashCombine(hash, key.Format);
    HashCombine(hash, key.Type);
    HashCombine(hash, key.WrapS);
    HashCombine(hash, key.WrapT);
    HashCombine(hash, key.MinLinear);
    HashCombine(hash, key.MagLinear);
    HashCombine(hash, key.MipLinear);
    HashCombine(hash, static_cast<uint16_t>(key.LodBiasRaw));
    HashCombine(hash, key.MinMipLevel);
    HashCombine(hash, key.MaxMipLevel);
    return hash;
}

size_t Oot3dNativePicaSubmissionQueue::GeometrySnapshotKeyHash::operator()(
    const GeometrySnapshotKey &key) const noexcept {
  size_t hash = 0;
  HashCombine(hash, static_cast<uint8_t>(key.Kind));
  HashCombine(hash, key.Slot);
  HashCombine(hash, key.PhysicalAddress);
  HashCombine(hash, key.Size);
  return hash;
}

bool Oot3dNativePicaSubmissionQueue::SubmitHardwareRegisterWrite(
    const Oot3dPicaHardwareRegisterWrite& write, std::string*) {
    std::lock_guard guard(mQueueMutex);
    mHardwareWrites.push_back(write);
    return true;
}

size_t Oot3dNativePicaSubmissionQueue::GeometryFrontIndex(
    const GeometrySnapshotKey& key) noexcept {
  const uint32_t mixed =
      (key.PhysicalAddress ^ static_cast<uint32_t>(key.Size) * 0x85EBCA6BU ^
       (static_cast<uint32_t>(key.Slot) << 1U | static_cast<uint32_t>(key.Kind))) *
      0x9E3779B1U;
  static_assert((kGeometryFrontSlots & (kGeometryFrontSlots - 1U)) == 0U);
  return mixed >> (32U - std::countr_zero(kGeometryFrontSlots));
}

void Oot3dNativePicaSubmissionQueue::ClearGeometryFront() noexcept {
  for (auto& slot : mGeometryFront) slot.Node = nullptr;
}

size_t Oot3dNativePicaSubmissionQueue::TextureFrontIndex(
    const TextureSnapshotKey& key) noexcept {
  const uint32_t shape =
      (static_cast<uint32_t>(key.Width) << 16U | key.Height) ^
      (static_cast<uint32_t>(key.Format) << 24U | static_cast<uint32_t>(key.MaxMipLevel) << 20U);
  const uint32_t mixed = (key.PhysicalAddress ^ shape * 0x85EBCA6BU) * 0x9E3779B1U;
  static_assert((kTextureFrontSlots & (kTextureFrontSlots - 1U)) == 0U);
  return mixed >> (32U - std::countr_zero(kTextureFrontSlots));
}

void Oot3dNativePicaSubmissionQueue::ClearTextureFront() noexcept {
  for (auto& slot : mTextureFront) slot.Node = nullptr;
}

bool Oot3dNativePicaSubmissionQueue::ComparePageRuns(
    const uint8_t* cached, std::span<const uint8_t> guest, uint32_t guestAddress,
    std::span<const uint64_t> kept, std::span<const uint64_t> current,
    uint64_t& comparedBytes, uint64_t& dirtyPages) {
  // Pages whose certificate did not change since the cached bytes were last
  // proven equal to memory still hold them (the version hit's proof, page by
  // page): compare the others, a run of adjacent ones in one call.
  const uint8_t* guestBytes = guest.data();
  const uint64_t firstPage = guestAddress >> 12U;
  size_t offset = 0U;
  size_t run = 0U;
  bool inRun = false;
  for (size_t page = 0; page <= current.size(); ++page) {
    const bool dirty = page < current.size() && kept[page] != current[page];
    if (dirty && !inRun) {
      run = offset;
      inRun = true;
    } else if (!dirty && inRun) {
      inRun = false;
      comparedBytes += offset - run;
      if (std::memcmp(cached + run, guestBytes + run, offset - run) != 0) return false;
    }
    if (page == current.size()) break;
    dirtyPages += dirty ? 1U : 0U;
    offset = static_cast<size_t>(std::min<uint64_t>(
        guest.size(), ((firstPage + page + 1U) << 12U) - guestAddress));
  }
  return true;
}

void Oot3dNativePicaSubmissionQueue::LinkGeometryNewest(
    std::pair<const GeometrySnapshotKey, GeometrySnapshotCacheEntry>& node) {
  GeometrySnapshotCacheEntry& entry = node.second;
  entry.LruKey = &node.first;
  entry.LruOlder = mGeometryLruNewest;
  entry.LruNewer = nullptr;
  if (mGeometryLruNewest != nullptr) {
    mGeometryLruNewest->LruNewer = &entry;
  } else {
    mGeometryLruOldest = &entry;
  }
  mGeometryLruNewest = &entry;
}

void Oot3dNativePicaSubmissionQueue::UnlinkGeometry(
    GeometrySnapshotCacheEntry& entry) {
  if (entry.LruKey == nullptr) return;  // not linked
  if (entry.LruOlder != nullptr) {
    entry.LruOlder->LruNewer = entry.LruNewer;
  } else {
    mGeometryLruOldest = entry.LruNewer;
  }
  if (entry.LruNewer != nullptr) {
    entry.LruNewer->LruOlder = entry.LruOlder;
  } else {
    mGeometryLruNewest = entry.LruOlder;
  }
  entry.LruOlder = entry.LruNewer = nullptr;
  entry.LruKey = nullptr;
}

void Oot3dNativePicaSubmissionQueue::RebuildGeometryLru() {
  mGeometryLruOldest = mGeometryLruNewest = nullptr;
  std::vector<std::pair<const GeometrySnapshotKey, GeometrySnapshotCacheEntry>*>
      order;
  order.reserve(mGeometrySnapshotCache.size());
  for (auto& node : mGeometrySnapshotCache) {
    node.second.LruOlder = node.second.LruNewer = nullptr;
    node.second.LruKey = nullptr;
    order.push_back(&node);
  }
  std::stable_sort(order.begin(), order.end(), [](const auto* left, const auto* right) {
    return left->second.LastUsedSequence < right->second.LastUsedSequence;
  });
  for (auto* node : order) LinkGeometryNewest(*node);
}

bool Oot3dNativePicaSubmissionQueue::CaptureGeometryResource(
    Oot3dPicaResourceKind kind, uint8_t slot, uint32_t physicalAddress,
    uint32_t firstElement, size_t byteCount,
    Oot3dPicaResourceSnapshot &resource, std::string *error,
    GeometrySnapshotCacheEntry **entryOut) {
  uint64_t ticks = CaptureTicks();
  ++mCounters.GeometryResources;
  // config/vertex_snapshot_fast: the view and the certificate as two direct
  // memory calls; aliased ranges keep the previous path.
  std::optional<std::span<const uint8_t>> source;
  std::optional<uint64_t> sourceWriteGeneration;
  uint32_t guestAddress = 0U;
  bool direct = false;
  bool unmapped = false;
  if (mVertexSnapshotFast) {
    std::span<const uint8_t> bytes;
    switch (mMemory.ViewCertified(physicalAddress, byteCount, bytes,
                                  guestAddress, sourceWriteGeneration)) {
    case Oot3dPicaPhysicalMemoryView::DirectView::Viewed:
      source = bytes;
      direct = true;
      break;
    case Oot3dPicaPhysicalMemoryView::DirectView::Unmapped:
      unmapped = true;
      break;
    case Oot3dPicaPhysicalMemoryView::DirectView::Fallback:
      ++mCounters.GeometryFallbacks;
      break;
    }
  }
  if (!direct && !unmapped) {
    source = mMemory.View(physicalAddress, byteCount);
    if (source.has_value()) {
      sourceWriteGeneration = SnapshotWriteGeneration(physicalAddress, byteCount);
    }
  }
  if (!source.has_value()) {
    SetError(error, kind == Oot3dPicaResourceKind::IndexBuffer
                        ? "PICA index buffer is outside mapped guest memory"
                        : "PICA vertex loader is outside mapped guest memory");
    return false;
  }
  if (!sourceWriteGeneration.has_value()) ++mCounters.GeometryUncertified;
  // A certified range over several pages: its entry keeps the certificate
  // of each page, taken when the range's changed.
  const size_t pageCount =
      direct ? static_cast<size_t>(((uint64_t{guestAddress} + byteCount - 1U) >> 12U) -
                                   (guestAddress >> 12U) + 1U)
             : 0U;
  const bool paged = sourceWriteGeneration.has_value() && pageCount > 1U;
  if (paged) mCounters.GeometryPages += pageCount;
  uint64_t now = CaptureTicks();
  mCounters.GeometryViewTicks += now - ticks;

  const GeometrySnapshotKey key{kind, slot, physicalAddress, byteCount};
  GeometrySnapshotNode *cached = nullptr;
  GeometryFrontSlot *front = nullptr;
  if (mVertexSnapshotFast) {
    front = &mGeometryFront[GeometryFrontIndex(key)];
    if (front->Node != nullptr && front->Key == key) cached = front->Node;
  }
  if (cached == nullptr) {
    const auto found = mGeometrySnapshotCache.find(key);
    if (found != mGeometrySnapshotCache.end()) {
      cached = &*found;
      if (front != nullptr) *front = {key, cached};
    }
  }
  const bool cacheEntryAvailable = cached != nullptr;
  const bool versionHit =
      cacheEntryAvailable && cached->second.Bytes != nullptr &&
      cached->second.Bytes->size() == source->size() &&
      sourceWriteGeneration.has_value() &&
      cached->second.SourceWriteGeneration == *sourceWriteGeneration;
  // The page certificates: after the range's (which took the cards of every
  // page), before any byte is compared or copied, so that a page whose
  // certificate is kept unchanged was not written after its bytes were read
  // (timed with the comparison).
  bool pages = false;
  if (paged && !versionHit) {
    const uint64_t pagesStart = CaptureTicks();
    pages = mMemory.GuestPageGenerations(guestAddress, byteCount, mPageStamps);
    mCounters.GeometryCompareTicks += CaptureTicks() - pagesStart;
  }
  const bool compared = cacheEntryAvailable && !versionHit;
  bool contentMatch = false;
  if (compared) {
    const uint64_t compareStart = CaptureTicks();
    const auto &entry = cached->second;
    mCounters.GeometryUnprovenBytes += source->size();
    if (entry.Bytes != nullptr) {
      if (entry.Bytes->size() == source->size()) {
        if (pages && entry.PageStamps.size() == mPageStamps.size()) {
          contentMatch = ComparePageRuns(entry.Bytes->data(), *source, guestAddress,
                                         entry.PageStamps, mPageStamps,
                                         mCounters.GeometryComparedBytes,
                                         mCounters.GeometryDirtyPages);
        } else {
          mCounters.GeometryComparedBytes += source->size();
          contentMatch = std::memcmp(entry.Bytes->data(), source->data(),
                                     source->size()) == 0;
        }
      }
    } else {
      contentMatch = entry.ContentHash == HashTimed(*source);
    }
    mCounters.GeometryCompareTicks += CaptureTicks() - compareStart;
  }
  const bool cacheHit =
      versionHit || contentMatch;
  if (compared && cached->second.Bytes == nullptr) mCounters.HashedBytes += source->size();
  if (versionHit) {
    ++mCounters.GeometryVersionHits;
  } else {
    ++mCounters.GeometryVersionMisses;
  }
  if (mRuntimeProfilingEnabled) {
    if (versionHit) {
      ++mRuntimeProfile.GeometrySnapshotVersionHits;
    } else {
      ++mRuntimeProfile.GeometrySnapshotVersionMisses;
    }
    if (compared) {
      mRuntimeProfile.GeometrySnapshotComparedBytes += source->size();
    }
    if (cacheHit) {
      ++mRuntimeProfile.GeometrySnapshotCacheHits;
    } else {
      ++mRuntimeProfile.GeometrySnapshotCacheMisses;
      mRuntimeProfile.GeometrySnapshotCopiedBytes += source->size();
    }
  }
  if (!cacheHit) {
    const uint64_t copyStart = CaptureTicks();
    if (cached == nullptr &&
        mGeometrySnapshotCache.size() >= kGeometrySnapshotCacheLimit &&
        mGeometryLruOldest != nullptr) {
      // The least recently used entry (the smallest LastUsedSequence), as
      // the linear scan this replaces chose it.
      GeometrySnapshotCacheEntry &oldest = *mGeometryLruOldest;
      const GeometrySnapshotKey oldestKey = *oldest.LruKey;
      UnlinkGeometry(oldest);
      auto &oldestFront = mGeometryFront[GeometryFrontIndex(oldestKey)];
      if (oldestFront.Node != nullptr && &oldestFront.Node->second == &oldest) {
        oldestFront.Node = nullptr;
      }
      mGeometrySnapshotCache.erase(oldestKey);
      ++mCounters.GeometryEvictions;
    }
    GeometrySnapshotCacheEntry entry;
    entry.Bytes = std::make_shared<const std::vector<uint8_t>>(source->begin(),
                                                               source->end());
    mCounters.CopiedBytes += source->size();
    ++mCounters.GeometryMisses;
    if (mLeanCapture) {
      // Only a savestate reads it (CaptureState hashes the bytes then).
      entry.ContentHashValid = false;
    } else {
      entry.ContentHash = HashTimed(*source);
      mCounters.HashedBytes += source->size();
    }
    entry.ContentVersion = mNextGeometryContentVersion++;
    entry.SourceWriteGeneration = sourceWriteGeneration.value_or(0U);
    if (mNextGeometryContentVersion == 0U) {
      mNextGeometryContentVersion = 1U;
    }
    if (cached != nullptr) {
      // Replaced in place: the entry keeps its node (and the capacity of its
      // page certificates) and is relinked below.
      UnlinkGeometry(cached->second);
      entry.PageStamps = std::move(cached->second.PageStamps);
      cached->second = std::move(entry);
    } else {
      cached = &*mGeometrySnapshotCache.emplace(key, std::move(entry)).first;
      if (front != nullptr) *front = {key, cached};
    }
    if (pages) {
      cached->second.PageStamps.assign(mPageStamps.begin(), mPageStamps.end());
    } else {
      cached->second.PageStamps.clear();
    }
    cached->second.LastUsedSequence = ++mGeometrySnapshotSequence;
    LinkGeometryNewest(*cached);
    mCounters.GeometryCopyTicks += CaptureTicks() - copyStart;
  } else {
    auto &entry = cached->second;
    if (entry.Bytes == nullptr) {
      const uint64_t copyStart = CaptureTicks();
      entry.Bytes =
          std::make_shared<const std::vector<uint8_t>>(source->begin(),
                                                       source->end());
      mCounters.CopiedBytes += source->size();
      mCounters.GeometryCopyTicks += CaptureTicks() - copyStart;
    }
    entry.SourceWriteGeneration = sourceWriteGeneration.value_or(0U);
    // On a version hit no page changed: the kept certificates stay valid.
    // Otherwise the bytes were proven against these (or none).
    if (pages) {
      entry.PageStamps.assign(mPageStamps.begin(), mPageStamps.end());
    } else if (!versionHit) {
      entry.PageStamps.clear();
    }
    entry.LastUsedSequence = ++mGeometrySnapshotSequence;
    UnlinkGeometry(entry);
    LinkGeometryNewest(*cached);
  }
  if (entryOut != nullptr) *entryOut = &cached->second;

  resource.Kind = kind;
  resource.Slot = slot;
  resource.PhysicalAddress = physicalAddress;
  resource.FirstElement = firstElement;
  resource.SharedBytes = cached->second.Bytes;
  resource.ContentVersion = cached->second.ContentVersion;
  resource.ContentVersionAvailable = true;
  return true;
}

bool Oot3dNativePicaSubmissionQueue::CaptureIndexAndVertexResources(
    Oot3dPicaDrawSubmission &submission, std::string *error) {
  const auto &vertex = submission.State.VertexInput;
  if (vertex.VertexCount == 0U) {
    return true;
  }

  // Index buffer, vertex loaders and the textures captured next: one
  // allocation instead of regrowing the snapshot vector on every draw.
  submission.Resources.reserve(submission.Resources.size() + 1U +
                               vertex.Loaders.size() +
                               submission.State.Textures.size());

  uint32_t minimumIndex = vertex.VertexOffset;
  uint32_t maximumIndex = 0;
  if (vertex.Indexed) {
    const size_t indexSize = vertex.IndicesAre16Bit ? 2U : 1U;
    if (vertex.VertexCount > std::numeric_limits<size_t>::max() / indexSize) {
      SetError(error, "PICA index buffer size overflows");
      return false;
    }
    Oot3dPicaResourceSnapshot indices;
    GeometrySnapshotCacheEntry* indexEntry = nullptr;
    if (!CaptureGeometryResource(
            Oot3dPicaResourceKind::IndexBuffer, 0U, vertex.IndexPhysicalAddress,
            0U, static_cast<size_t>(vertex.VertexCount) * indexSize, indices,
            error, &indexEntry)) {
      return false;
    }
    const auto indexBytes = indices.ResolvedBytes();
    const uint8_t rangeKind = vertex.IndicesAre16Bit ? 2U : 1U;
    // The range is a function of the cached bytes (the snapshot reads
    // exactly those) and of the index width: remembered with the entry.
    if (mLeanCapture && indexEntry != nullptr &&
        indexEntry->IndexRangeKind == rangeKind &&
        indexEntry->Bytes != nullptr &&
        indexEntry->Bytes->data() == indexBytes.data()) {
      minimumIndex = indexEntry->IndexMinimum;
      maximumIndex = indexEntry->IndexMaximum;
      ++mCounters.IndexRangeReuses;
    } else {
      minimumIndex = std::numeric_limits<uint32_t>::max();
      maximumIndex = 0U;
      for (size_t index = 0; index < vertex.VertexCount; ++index) {
        const uint32_t value =
            vertex.IndicesAre16Bit
                ? static_cast<uint32_t>(indexBytes[index * 2U]) |
                      (static_cast<uint32_t>(indexBytes[index * 2U + 1U]) << 8U)
                : indexBytes[index];
        minimumIndex = std::min(minimumIndex, value);
        maximumIndex = std::max(maximumIndex, value);
      }
      if (mLeanCapture && indexEntry != nullptr && indexEntry->Bytes != nullptr &&
          indexEntry->Bytes->data() == indexBytes.data()) {
        indexEntry->IndexRangeKind = rangeKind;
        indexEntry->IndexMinimum = minimumIndex;
        indexEntry->IndexMaximum = maximumIndex;
      }
    }
    submission.Resources.push_back(std::move(indices));
  } else {
    const uint64_t maximum =
        static_cast<uint64_t>(vertex.VertexOffset) + vertex.VertexCount - 1U;
    if (maximum > std::numeric_limits<uint32_t>::max()) {
      SetError(error, "PICA non-indexed vertex range overflows");
      return false;
    }
    maximumIndex = static_cast<uint32_t>(maximum);
  }
  submission.MinimumVertexIndex = minimumIndex;
  submission.MaximumVertexIndex = maximumIndex;

  const uint64_t elementCount =
      static_cast<uint64_t>(maximumIndex) - minimumIndex + 1U;
  for (size_t loaderIndex = 0; loaderIndex < vertex.Loaders.size();
       ++loaderIndex) {
    const auto &loader = vertex.Loaders[loaderIndex];
    if (loader.ComponentCount == 0U || loader.ByteStride == 0U) {
      continue;
    }
    const uint64_t byteOffset =
        static_cast<uint64_t>(minimumIndex) * loader.ByteStride;
    const uint64_t byteCount = elementCount * loader.ByteStride;
    const uint64_t physicalAddress = loader.PhysicalAddress + byteOffset;
    if (physicalAddress > std::numeric_limits<uint32_t>::max() ||
        byteCount > std::numeric_limits<size_t>::max()) {
      SetError(error, "PICA vertex loader snapshot range overflows");
      return false;
    }
    Oot3dPicaResourceSnapshot resource;
    if (!CaptureGeometryResource(Oot3dPicaResourceKind::VertexLoader,
                                 static_cast<uint8_t>(loaderIndex),
                                 static_cast<uint32_t>(physicalAddress),
                                 minimumIndex, static_cast<size_t>(byteCount),
                                 resource, error)) {
      return false;
    }
    submission.Resources.push_back(std::move(resource));
  }
  return true;
}

void Oot3dNativePicaSubmissionQueue::BindGuestWriteObservation(
    GuestWriteRegistrar registrar) {
    std::lock_guard guard(mQueueMutex);
    mGuestWriteRegistrar = std::move(registrar);
    ResetSnapshotObservation();
}

bool Oot3dNativePicaSubmissionQueue::HasGuestWriteObservation() const noexcept {
    std::lock_guard guard(mQueueMutex);
    return static_cast<bool>(mGuestWriteRegistrar);
}

void Oot3dNativePicaSubmissionQueue::NotifyGuestWrite(uint32_t guestAddress,
                                                      size_t byteCount) {
    if (byteCount == 0U) {
        return;
    }
    std::lock_guard guard(mQueueMutex);
    if (mRawCopyProducers.empty()) {
        return;
    }
    // A write may cross the boundary of a mapped view: attribute every mapped
    // segment instead of dropping the notification.
    uint32_t current = guestAddress;
    size_t remaining = byteCount;
    bool attributed = false;
    while (remaining != 0U) {
        const auto segment = mMemory.GuestSegment(current, remaining);
        if (!segment.has_value()) {
            break;
        }
        const auto physical =
            mMemory.TranslateGuest(segment->first, segment->second);
        if (!physical.has_value()) {
            break;
        }
        InvalidateRawCopyOwnership(*physical, segment->second);
        attributed = true;
        current += static_cast<uint32_t>(segment->second);
        remaining -= segment->second;
    }
    if (!attributed || remaining != 0U) {
        // A write that cannot be attributed to any physical range may still
        // cover an owned subrange: ownership that cannot be certified is
        // dropped instead of being assumed.
        mRawCopyProducers.clear();
        ResetSnapshotObservation();
    }
}


void Oot3dNativePicaSubmissionQueue::CollectLiveCopyReferences(
    std::vector<Fast::Renderer3ds::PicaTextureCopyReference>& out) const {
    std::lock_guard guard(mQueueMutex);
    // Newest producer first, the same order a snapshot resolves them in.
    for (auto producer = mRawCopyProducers.rbegin();
         producer != mRawCopyProducers.rend(); ++producer) {
        for (const auto& span : producer->Owned) {
            out.push_back({producer->CompletionId,
                           producer->AfterDrawSubmissionId, span.Address,
                           span.ByteCount});
        }
    }
}

bool Oot3dNativePicaSubmissionQueue::ObservePhysicalWriteSpan(
    uint32_t address, size_t byteCount) {
    return mGuestWriteRegistrar &&
        mMemory.ForEachGuestAlias(address, byteCount, mGuestWriteRegistrar);
}

void Oot3dNativePicaSubmissionQueue::ResetSnapshotObservation() {
    for (auto& [key, entry] : mGeometrySnapshotCache) {
        entry.SourceWriteGeneration = 0;
        entry.PageStamps.clear();
    }
    for (auto& [key, entry] : mTextureSnapshotCache) {
        entry.SourceWriteGeneration = 0;
        entry.PageStamps.clear();
    }
}

std::optional<uint64_t> Oot3dNativePicaSubmissionQueue::SnapshotWriteGeneration(
    uint32_t address, size_t byteCount) {
    std::optional<uint32_t> first;
    bool aliased = false;
    if (!mMemory.ForEachGuestAlias(address, byteCount, [&](uint32_t guest, size_t) {
            aliased = aliased || (first && *first != guest);
            first = guest;
            return true;
        })) return std::nullopt;
    // Distinct backing aliases still need byte synchronization. A single
    // mapping needs no callback or write trap for cache version tracking.
    if (aliased && !ObservePhysicalWriteSpan(address, byteCount)) return std::nullopt;
    return mMemory.RangeWriteGeneration(address, byteCount);
}


void Oot3dNativePicaSubmissionQueue::SubtractOwnedSpan(
    std::vector<RawCopySpan>& spans, uint32_t address, size_t byteCount) {
    if (byteCount == 0U || spans.empty()) {
        return;
    }
    const uint64_t removedLow = address;
    const uint64_t removedHigh = removedLow + byteCount;
    std::vector<RawCopySpan> kept;
    kept.reserve(spans.size() + 1U);
    for (const auto& span : spans) {
        const uint64_t spanLow = span.Address;
        const uint64_t spanHigh = spanLow + span.ByteCount;
        if (spanHigh <= removedLow || spanLow >= removedHigh) {
            kept.push_back(span);
            continue;
        }
        if (spanLow < removedLow) {
            kept.push_back(
                {span.Address,
                 static_cast<uint32_t>(removedLow - spanLow)});
        }
        if (spanHigh > removedHigh) {
            kept.push_back(
                {static_cast<uint32_t>(removedHigh),
                 static_cast<uint32_t>(spanHigh - removedHigh)});
        }
    }
    spans = std::move(kept);
}

void Oot3dNativePicaSubmissionQueue::InvalidateRawCopyOwnership(
    uint32_t physicalAddress, size_t byteCount) {
    for (auto& producer : mRawCopyProducers) {
        SubtractOwnedSpan(producer.Owned, physicalAddress, byteCount);
    }
    std::erase_if(mRawCopyProducers, [](const RawCopyProducer& producer) {
        return producer.Owned.empty();
    });
}

bool Oot3dNativePicaSubmissionQueue::RegisterRawCopyProducer(
    uint64_t completionId, uint64_t afterDrawSubmissionId,
    std::span<const RawCopySpan> destinations, std::string* error) {
    if (completionId == 0U) {
        SetError(error, "raw PICA texture copy has no completion identity");
        return false;
    }
    // A replay of the same transfer is the same producer, never a second owner.
    for (const auto& producer : mRawCopyProducers) {
        if (producer.CompletionId == completionId) {
            return true;
        }
    }
    if (!mGuestWriteRegistrar) {
        SetError(error,
                 "raw PICA texture copy requires guest write observation");
        return false;
    }
    RawCopyProducer producer;
    producer.CompletionId = completionId;
    producer.AfterDrawSubmissionId = afterDrawSubmissionId;
    for (const auto& span : destinations) {
        if (span.ByteCount == 0U) {
            continue;
        }
        if (!ObservePhysicalWriteSpan(span.Address, span.ByteCount)) {
            SetError(error,
                     "raw PICA texture copy destination cannot be observed: an "
                     "escaped writable pointer overlaps it");
            return false;
        }
        producer.Owned.push_back(span);
    }
    if (producer.Owned.empty()) {
        SetError(error, "raw PICA texture copy owns no destination bytes");
        return false;
    }
    // The newest copy takes its bytes back from every older producer.
    for (auto& existing : mRawCopyProducers) {
        for (const auto& span : producer.Owned) {
            SubtractOwnedSpan(existing.Owned, span.Address, span.ByteCount);
        }
    }
    std::erase_if(mRawCopyProducers, [](const RawCopyProducer& existing) {
        return existing.Owned.empty();
    });
    mRawCopyProducers.push_back(std::move(producer));
    return true;
}

void Oot3dNativePicaSubmissionQueue::CollectCopyReferences(
    uint64_t drawSubmissionId, uint32_t physicalAddress, size_t byteCount,
    std::vector<Fast::Renderer3ds::PicaTextureCopyReference>& out) const {
    out.clear();
    if (byteCount == 0U || byteCount > std::numeric_limits<uint32_t>::max()) {
        return;
    }
    const uint32_t textureLow = physicalAddress;
    const uint32_t textureHigh =
        static_cast<uint32_t>(physicalAddress + byteCount);
    std::vector<RawCopySpan> claimed;
    uint64_t claimedBytes = 0U;
    // Newest producer first: the latest copy owns every byte it still holds.
    for (auto producer = mRawCopyProducers.rbegin();
         producer != mRawCopyProducers.rend(); ++producer) {
        if (producer->AfterDrawSubmissionId >= drawSubmissionId) {
            continue;
        }
        for (const auto& span : producer->Owned) {
            const uint32_t low = std::max(span.Address, textureLow);
            const uint32_t high = std::min(
                static_cast<uint32_t>(span.Address + span.ByteCount),
                textureHigh);
            if (low >= high) {
                continue;
            }
            std::vector<RawCopySpan> remaining{{low,
                                                static_cast<uint32_t>(high - low)}};
            for (const auto& taken : claimed) {
                SubtractOwnedSpan(remaining, taken.Address, taken.ByteCount);
            }
            for (const auto& piece : remaining) {
                out.push_back({producer->CompletionId,
                               producer->AfterDrawSubmissionId,
                               piece.Address, piece.ByteCount});
                claimed.push_back(piece);
                claimedBytes += piece.ByteCount;
            }
        }
        if (claimedBytes >= byteCount) {
            break;
        }
    }
}

bool Oot3dNativePicaSubmissionQueue::CaptureTextureResources(
    Oot3dPicaDrawSubmission& submission, std::string* error) {
    for (size_t textureIndex = 0;
         textureIndex < submission.State.Textures.size(); ++textureIndex) {
        const auto& texture = submission.State.Textures[textureIndex];
        if (!texture.Enabled) {
            continue;
        }
        const uint64_t viewStart = CaptureTicks();
        const auto byteCount = Oot3dPicaTextureMipChainByteSize(texture);
        if (!byteCount.has_value()) {
            SetError(error, "PICA texture has an invalid native format or size");
            return false;
        }
        Oot3dPicaResourceSnapshot resource;
        resource.Kind = Oot3dPicaResourceKind::Texture;
        resource.Slot = static_cast<uint8_t>(textureIndex);
        resource.PhysicalAddress = texture.PhysicalAddress;
        CollectCopyReferences(submission.Id, resource.PhysicalAddress,
                              *byteCount, resource.CopyReferences);
        if (!resource.CopyReferences.empty()) {
            ++mCounters.TextureCopyReferenced;
            // Never read/hash a promised GPU write to build its own sampler.
            // Keep only CPU-owned gaps in an immutable base; referenced slots
            // are deliberately absent content, replaced by the GPU derivation.
            std::sort(resource.CopyReferences.begin(), resource.CopyReferences.end(),
                [](const auto& a, const auto& b) { return a.PhysicalAddress < b.PhysicalAddress; });
            auto base = std::make_shared<std::vector<uint8_t>>(*byteCount, 0U);
            uint64_t next = resource.PhysicalAddress;
            const uint64_t end = next + *byteCount;
            const auto copyGap = [&](uint64_t high) {
                if (next >= high) return true;
                return mMemory.Read(static_cast<uint32_t>(next),
                    std::span<uint8_t>(*base).subspan(
                        static_cast<size_t>(next - resource.PhysicalAddress),
                        static_cast<size_t>(high - next)));
            };
            for (const auto& reference : resource.CopyReferences) {
                if (!copyGap(reference.PhysicalAddress)) {
                    SetError(error, "PICA copied texture has an unmapped CPU-owned gap");
                    return false;
                }
                next = uint64_t{reference.PhysicalAddress} + reference.ByteCount;
            }
            if (!copyGap(end)) {
                SetError(error, "PICA copied texture has an unmapped CPU-owned tail");
                return false;
            }
            resource.SharedBytes = std::move(base);
            if (mRuntimeProfilingEnabled) {
                mRuntimeProfile.TextureCopyReferences += resource.CopyReferences.size();
                for (const auto& reference : resource.CopyReferences)
                    mRuntimeProfile.TextureCopyReferencedBytes += reference.ByteCount;
            }
            // Asset replacement/HD-font transforms require native CPU content;
            // a device-produced version is not an immutable title asset.
            submission.Resources.push_back(std::move(resource));
            continue;
        }
        ++mCounters.TextureResources;
        // config/texture_snapshot_fast: the view and the certificate as two
        // direct memory calls; aliased ranges keep the previous path.
        std::optional<std::span<const uint8_t>> source;
        std::optional<uint64_t> sourceWriteGeneration;
        uint32_t guestAddress = 0U;
        bool direct = false;
        bool unmapped = false;
        if (mTextureSnapshotFast) {
            std::span<const uint8_t> bytes;
            switch (mMemory.ViewCertified(resource.PhysicalAddress, *byteCount, bytes,
                                          guestAddress, sourceWriteGeneration)) {
            case Oot3dPicaPhysicalMemoryView::DirectView::Viewed:
                source = bytes;
                direct = true;
                break;
            case Oot3dPicaPhysicalMemoryView::DirectView::Unmapped:
                unmapped = true;
                break;
            case Oot3dPicaPhysicalMemoryView::DirectView::Fallback:
                ++mCounters.TextureFallbacks;
                break;
            }
        }
        if (!direct && !unmapped) {
            source = mMemory.View(resource.PhysicalAddress, *byteCount);
        }
        if (!source.has_value()) {
            SetError(error, "PICA texture is outside mapped guest memory");
            return false;
        }
        if (!direct) {
            sourceWriteGeneration =
                SnapshotWriteGeneration(resource.PhysicalAddress, *byteCount);
        }
        if (!sourceWriteGeneration.has_value()) ++mCounters.TextureUncertified;
        // A certified texture over several pages: its entry keeps the
        // certificate of each page, taken when the texture's changed.
        const size_t pageCount =
            direct ? static_cast<size_t>(((uint64_t{guestAddress} + *byteCount - 1U) >> 12U) -
                                         (guestAddress >> 12U) + 1U)
                   : 0U;
        const bool paged = sourceWriteGeneration.has_value() && pageCount > 1U;
        if (paged) mCounters.TexturePages += pageCount;
        const TextureSnapshotKey key{
            texture.PhysicalAddress,
            texture.Width,
            texture.Height,
            texture.Format,
            texture.Type,
            texture.WrapS,
            texture.WrapT,
            texture.MinLinear,
            texture.MagLinear,
            texture.MipLinear,
            texture.LodBiasRaw,
            texture.MinMipLevel,
            texture.MaxMipLevel,
        };
        TextureSnapshotNode* cached = nullptr;
        TextureFrontSlot* front = nullptr;
        if (mTextureSnapshotFast) {
            front = &mTextureFront[TextureFrontIndex(key)];
            if (front->Node != nullptr && front->Key == key) cached = front->Node;
        }
        if (cached == nullptr) {
            const auto found = mTextureSnapshotCache.find(key);
            if (found != mTextureSnapshotCache.end()) {
                cached = &*found;
                if (front != nullptr) *front = {key, cached};
            }
        }
        const bool cacheMetadataAvailable = cached != nullptr;
        const bool versionHit = cacheMetadataAvailable && sourceWriteGeneration.has_value() &&
            cached->second.SourceBytes != nullptr &&
            cached->second.SourceBytes->size() == source->size() &&
            cached->second.SourceWriteGeneration == *sourceWriteGeneration;
        const uint64_t compareStart = CaptureTicks();
        mCounters.TextureViewTicks += compareStart - viewStart;
        // The page certificates: after the texture's (which took the cards
        // of every page), before any byte is compared or copied.
        bool pages = false;
        if (paged && !versionHit) {
            pages = mMemory.GuestPageGenerations(guestAddress, *byteCount, mPageStamps);
        }
        const bool compared = cacheMetadataAvailable && !versionHit;
        bool cacheHit = versionHit;
        if (compared) {
            const auto& entry = cached->second;
            mCounters.TextureUnprovenBytes += source->size();
            if (entry.SourceBytes != nullptr) {
                if (entry.SourceBytes->size() == source->size()) {
                    if (pages && entry.PageStamps.size() == mPageStamps.size()) {
                        cacheHit = ComparePageRuns(entry.SourceBytes->data(), *source,
                                                   guestAddress, entry.PageStamps, mPageStamps,
                                                   mCounters.TextureComparedBytes,
                                                   mCounters.TextureDirtyPages);
                    } else {
                        mCounters.TextureComparedBytes += source->size();
                        cacheHit = std::memcmp(entry.SourceBytes->data(), source->data(),
                                               source->size()) == 0;
                    }
                }
            } else {
                cacheHit = entry.SourceHash == HashTimed(*source);
            }
        }
        if (versionHit) {
            ++mCounters.TextureVersionHits;
        } else {
            ++mCounters.TextureVersionMisses;
        }
        const uint64_t copyStart = CaptureTicks();
        mCounters.TextureCompareTicks += copyStart - compareStart;
        if (mRuntimeProfilingEnabled) {
            if (compared) mRuntimeProfile.TextureSnapshotComparedBytes += source->size();
            if (cacheHit) {
                ++mRuntimeProfile.TextureSnapshotCacheHits;
            } else {
                ++mRuntimeProfile.TextureSnapshotCacheMisses;
                mRuntimeProfile.TextureSnapshotCopiedBytes += source->size();
            }
        }
        if (!cacheHit || cached->second.ResolvedBytes == nullptr) {
            auto sourceBytes =
                std::make_shared<std::vector<uint8_t>>(
                    source->begin(), source->end());
            std::shared_ptr<const std::vector<uint8_t>> resolvedBytes =
                sourceBytes;
            if (mTexturePayloadTransform) {
                auto transformedBytes =
                    std::make_shared<std::vector<uint8_t>>(*sourceBytes);
                const auto baseLevelBytes =
                    Oot3dPicaTextureMipLevelByteSize(texture, 0U);
                if (!baseLevelBytes.has_value() ||
                    *baseLevelBytes > transformedBytes->size()) {
                    SetError(error, "PICA texture base mip is invalid");
                    return false;
                }
                mTexturePayloadTransform(
                    texture,
                    std::span<uint8_t>(*transformedBytes)
                        .first(*baseLevelBytes));
                resolvedBytes = std::move(transformedBytes);
            }
            TextureSnapshotCacheEntry entry;
            entry.SourceBytes = std::move(sourceBytes);
            entry.ResolvedBytes = std::move(resolvedBytes);
            mCounters.CopiedBytes += entry.SourceBytes->size();
            const auto baseLevelBytes = Oot3dPicaTextureMipLevelByteSize(texture, 0U);
            if (mLeanCapture && baseLevelBytes.has_value() &&
                *baseLevelBytes <= entry.ResolvedBytes->size()) {
                // One pass: FNV-1a is sequential, so the base level's hash is
                // the state after its bytes, and without a payload transform
                // the resolved bytes are the source bytes.
                const std::span<const uint8_t> resolved(*entry.ResolvedBytes);
                entry.BaseLevelContentHash =
                    HashTimed(resolved.first(*baseLevelBytes));
                entry.ContentHash = ContinueHashTimed(
                    entry.BaseLevelContentHash, resolved.subspan(*baseLevelBytes));
                mCounters.HashedBytes += resolved.size();
                if (entry.SourceBytes == entry.ResolvedBytes) {
                    entry.SourceHash = entry.ContentHash;
                } else {
                    entry.SourceHash = HashTimed(*entry.SourceBytes);
                    mCounters.HashedBytes += entry.SourceBytes->size();
                }
            } else {
            entry.SourceHash = HashTimed(*entry.SourceBytes);
            entry.ContentHash = HashTimed(*entry.ResolvedBytes);
            mCounters.HashedBytes += entry.SourceBytes->size() + entry.ResolvedBytes->size();
            if (!baseLevelBytes.has_value() || *baseLevelBytes > entry.ResolvedBytes->size()) {
                SetError(error, "PICA texture base mip is invalid");
                return false;
            }
            // Both versions belong to the immutable, transformed payload.
            // Savestate restore rebuilds that payload and these derived hashes.
            entry.BaseLevelContentHash = *baseLevelBytes == entry.ResolvedBytes->size()
                ? entry.ContentHash
                : HashTimed(std::span<const uint8_t>(*entry.ResolvedBytes).first(*baseLevelBytes));
            if (*baseLevelBytes != entry.ResolvedBytes->size())
                mCounters.HashedBytes += *baseLevelBytes;
            }
            if (pages) entry.PageStamps.assign(mPageStamps.begin(), mPageStamps.end());
            ++mCounters.TextureMisses;
            cached = &*mTextureSnapshotCache.insert_or_assign(
                key, std::move(entry)).first;
            if (front != nullptr) *front = {key, cached};
        } else if (pages) {
            // Proven equal to memory against these certificates.
            cached->second.PageStamps.assign(mPageStamps.begin(), mPageStamps.end());
        } else if (!versionHit) {
            // On a version hit no page changed: the kept certificates stay
            // valid; otherwise the bytes were proven against none.
            cached->second.PageStamps.clear();
        }
        mCounters.TextureCopyTicks += CaptureTicks() - copyStart;
        cached->second.SourceWriteGeneration = sourceWriteGeneration.value_or(0U);
        resource.SharedBytes = cached->second.ResolvedBytes;
        resource.ContentHash = cached->second.ContentHash;
        resource.ContentHashAvailable = true;
        resource.BaseLevelContentHash = cached->second.BaseLevelContentHash;
        resource.BaseLevelContentHashAvailable = true;
        if (mTextureSnapshotTransform) mTextureSnapshotTransform(texture, resource);
        submission.Resources.push_back(std::move(resource));
    }
    return true;
}

namespace {

// The packet's own fields as a source: a copied packet takes the same path
// as the frontend's source (the same bytes, TestPacketSource).
Oot3dPicaDrawPacketSource SourceOfPacket(const Oot3dPicaDrawPacket& packet) {
    Oot3dPicaDrawPacketSource source;
    source.CommandListAddress = packet.CommandListAddress;
    source.CommandListOffsetWords = packet.CommandListOffsetWords;
    source.CompositionDomain = packet.CompositionDomain;
    source.Composition = packet.Composition;
    source.LightBinding = &packet.LightBinding;
    source.Indexed = packet.Indexed;
    source.Registers = &packet.Registers;
    source.VertexShader = &packet.VertexShader;
    source.GeometryShader = &packet.GeometryShader;
    source.FogLut = &packet.FogLut;
    source.ProcTexLuts = &packet.ProcTexLuts;
    source.LightingLuts = packet.LightingLuts;
    source.DefaultAttributes = &packet.DefaultAttributes;
    return source;
}

constexpr size_t kPacketStateBytes =
    sizeof(Oot3dPicaDrawPacket::Registers) +
    2U * sizeof(Oot3dPicaShaderState::FloatUniforms) +
    sizeof(Oot3dPicaDrawPacket::DefaultAttributes);

} // namespace

void Oot3dNativePicaSubmissionQueue::SetTextureSnapshotFast(bool enabled) {
    std::lock_guard guard(mQueueMutex);
    mTextureSnapshotFast = enabled;
}

void Oot3dNativePicaSubmissionQueue::SetVertexSnapshotFast(bool enabled) {
    std::lock_guard guard(mQueueMutex);
    mVertexSnapshotFast = enabled;
}

void Oot3dNativePicaSubmissionQueue::SetCaptureDelta(bool enabled) {
    std::lock_guard guard(mQueueMutex);
    MaterializePendingLocked();
    mCaptureDelta = enabled;
}

void Oot3dNativePicaSubmissionQueue::MaterializePendingLocked() {
    if (mDeltaLog == nullptr || mDeltaLog->Entries() == 0U) return;
    mDeltaLog->MaterializeAll(mPendingDraws);
    mDeltaLog->Reset();
}

uint64_t Oot3dNativePicaSubmissionQueue::HashTimed(
    std::span<const uint8_t> bytes) {
    const uint64_t started = CaptureTicks();
    const uint64_t hash = HashSnapshotBytes(bytes);
    mCounters.HashTicks += CaptureTicks() - started;
    return hash;
}

uint64_t Oot3dNativePicaSubmissionQueue::ContinueHashTimed(
    uint64_t hash, std::span<const uint8_t> bytes) {
    const uint64_t started = CaptureTicks();
    hash = ContinueSnapshotHash(hash, bytes);
    mCounters.HashTicks += CaptureTicks() - started;
    return hash;
}

bool Oot3dNativePicaSubmissionQueue::SubmitDrawPacket(
    const Oot3dPicaDrawPacket& packet, std::string* error) {
    return SubmitDrawPacketSource(SourceOfPacket(packet), error);
}

bool Oot3dNativePicaSubmissionQueue::SubmitDrawPacketSource(
    const Oot3dPicaDrawPacketSource& source, std::string* error) {
    CpuPhaseProbe::Scope cpuLock(CpuPhaseProbe::Phase::QueueLock);
    std::lock_guard guard(mQueueMutex);
    cpuLock.Stop();
    const auto countersStart = RuntimeProfileClock::now();
    const auto totalStart =
        mRuntimeProfilingEnabled ? RuntimeProfileClock::now()
                                 : RuntimeProfileClock::time_point{};
    if (mRuntimeProfilingEnabled) {
        ++mRuntimeProfile.DrawCalls;
    }
    const auto phaseStart =
        mRuntimeProfilingEnabled ? RuntimeProfileClock::now()
                                 : RuntimeProfileClock::time_point{};
    // The packet is made from the frontend state right here, where the
    // queue keeps it; its register file, float uniforms and default
    // attributes are copied next (or recorded as a delta after the capture).
    // config/texture_snapshot_fast: a delta draw's shared tables are
    // recorded by the log too (once per change, not one reference each per
    // draw on this thread).
    const bool delta = mCaptureDelta && source.DirtyLines != nullptr;
    const bool deferTables = delta && mTextureSnapshotFast;
    uint64_t ticks = CaptureTicks();
    auto& submission =
        deferTables ? mPendingDraws.emplace_back(mNextSubmissionId, source,
                                                 Oot3dPicaStateArraysDeferred{},
                                                 Oot3dPicaStateTablesDeferred{})
                    : mPendingDraws.emplace_back(mNextSubmissionId, source,
                                                 Oot3dPicaStateArraysDeferred{});
    uint64_t now = CaptureTicks();
    mCounters.PacketTicks += now - ticks;
    if (!delta) {
        ticks = now;
        submission.Packet.Registers = *source.Registers;
        now = CaptureTicks();
        mCounters.RegisterTicks += now - ticks;
        ticks = now;
        submission.Packet.VertexShader.FloatUniforms =
            source.VertexShader->FloatUniforms;
        submission.Packet.GeometryShader.FloatUniforms =
            source.GeometryShader->FloatUniforms;
        submission.Packet.DefaultAttributes = *source.DefaultAttributes;
        mCounters.UniformTicks += CaptureTicks() - ticks;
        mCounters.StateBytes += kPacketStateBytes;
    }
    if (mRuntimeProfilingEnabled) {
        mRuntimeProfile.PacketCopyNanoseconds +=
            RuntimeProfileElapsedNanoseconds(phaseStart);
    }
    const bool submitted = SubmitDrawLocked(submission, source, error);
    if (submitted && delta) {
        // Only what changed since the previous entry (everything for the
        // first entry of a log); the consumer materializes the arrays.
        if (mDeltaLog == nullptr) {
            mDeltaLog = Oot3dPicaCaptureDeltaLogPool::Acquire(mDeltaPool);
        }
        Oot3dPicaCaptureDeltaLog::AppendCost cost;
        const uint32_t entry = mDeltaLog->Append(source, cost, deferTables);
        mPendingDraws.back().DeltaEntry = entry;
        mCounters.PacketTicks += cost.TableTicks;
        mCounters.RegisterTicks += cost.RegisterTicks;
        mCounters.UniformTicks += cost.UniformTicks;
        mCounters.StateBytes += cost.Bytes;
        if (mDeltaLog->Keyframe(entry)) {
            ++mCounters.Keyframes;
        } else {
            ++mCounters.DeltaDraws;
        }
    }
    if (mRuntimeProfilingEnabled) {
        mRuntimeProfile.TotalNanoseconds +=
            RuntimeProfileElapsedNanoseconds(totalStart);
    }
    ++mCounters.Draws;
    // config/texture_snapshot_fast: the lines the next draw's packet and
    // decoded state will be written to, fetched for writing while the list
    // walk runs (the slot was last written frames ago, often on another
    // core).
    if (mTextureSnapshotFast && !mPendingDraws.empty() &&
        mPendingDraws.size() < mPendingDraws.capacity()) {
        PrefetchNextDraw();
    }
    mCounters.Nanoseconds += RuntimeProfileElapsedNanoseconds(countersStart);
    return submitted;
}

void Oot3dNativePicaSubmissionQueue::PrefetchNextDraw() noexcept {
    const auto& last = mPendingDraws.back();
    const auto* base = reinterpret_cast<const char*>(&last);
    // The slot after `last`, inside the vector's capacity: its parts the
    // packet constructor and the decoder write (not the register file, the
    // float uniforms and the default attributes a capture-delta draw leaves
    // unset).
    const auto* next = base + sizeof(Oot3dPicaDrawSubmission);
    const auto prefetch = [base, next](const void* begin, const void* end) {
        const size_t first = static_cast<size_t>(static_cast<const char*>(begin) - base) & ~size_t{63U};
        const size_t limit = static_cast<size_t>(static_cast<const char*>(end) - base);
        for (size_t offset = first; offset < limit; offset += 64U) __builtin_prefetch(next + offset, 1, 3);
    };
    const auto& packet = last.Packet;
    prefetch(&last, &packet.Registers);
    for (const auto* shader : {&packet.VertexShader, &packet.GeometryShader}) {
        prefetch(shader, &shader->FloatUniforms);
        prefetch(&shader->IntegerUniforms, shader + 1);
    }
    prefetch(&packet.FogLut, &packet.DefaultAttributes);
    prefetch(&packet.DefaultAttributes + 1, &last + 1);
}

bool Oot3dNativePicaSubmissionQueue::SubmitDrawLocked(
    Oot3dPicaDrawSubmission& submission,
    const Oot3dPicaDrawPacketSource& source, std::string* error) {
    auto phaseStart =
        mRuntimeProfilingEnabled ? RuntimeProfileClock::now()
                                 : RuntimeProfileClock::time_point{};
    // From the frontend's registers: the same words the packet holds (or
    // will hold, with capture delta).
    uint64_t ticks = CaptureTicks();
    if (!DecodeOot3dPicaDrawState(*source.Registers, source.Indexed,
                                  submission.State, error)) {
        mPendingDraws.pop_back();
        return false;
    }
    uint64_t now = CaptureTicks();
    mCounters.DecodeTicks += now - ticks;
    ticks = now;
    if (mRuntimeProfilingEnabled) {
        mRuntimeProfile.DecodeNanoseconds +=
            RuntimeProfileElapsedNanoseconds(phaseStart);
        phaseStart = RuntimeProfileClock::now();
    }
    CpuPhaseProbe::Scope cpuVertex(CpuPhaseProbe::Phase::VertexCapture);
    if (!CaptureIndexAndVertexResources(submission, error)) {
        mPendingDraws.pop_back();
        return false;
    }
    cpuVertex.Stop();
    now = CaptureTicks();
    mCounters.VertexTicks += now - ticks;
    ticks = now;
    if (mRuntimeProfilingEnabled) {
        mRuntimeProfile.VertexCaptureNanoseconds +=
            RuntimeProfileElapsedNanoseconds(phaseStart);
        phaseStart = RuntimeProfileClock::now();
    }
    CpuPhaseProbe::Scope cpuTexture(CpuPhaseProbe::Phase::TextureCapture);
    if (!CaptureTextureResources(submission, error)) {
        mPendingDraws.pop_back();
        return false;
    }
    cpuTexture.Stop();
    now = CaptureTicks();
    mCounters.TextureTicks += now - ticks;
    ticks = now;
    if (mRuntimeProfilingEnabled) {
        mRuntimeProfile.TextureCaptureNanoseconds +=
            RuntimeProfileElapsedNanoseconds(phaseStart);
        for (const auto& resource : submission.Resources) {
            auto* byteCount = &mRuntimeProfile.VertexBytes;
            if (resource.Kind == Oot3dPicaResourceKind::IndexBuffer) {
                byteCount = &mRuntimeProfile.IndexBytes;
            } else if (resource.Kind == Oot3dPicaResourceKind::Texture) {
                byteCount = &mRuntimeProfile.TextureBytes;
            }
            *byteCount += resource.ResolvedBytes().size();
        }
        phaseStart = RuntimeProfileClock::now();
    }
    ++mNextSubmissionId;
    phaseStart =
        mRuntimeProfilingEnabled ? RuntimeProfileClock::now()
                                 : RuntimeProfileClock::time_point{};
    const uint32_t colorTarget =
        submission.State.Framebuffer.ColorPhysicalAddress;
    mKnownColorRenderTargets.insert(colorTarget);
    mInvalidatedColorRenderTargets.erase(colorTarget);
    for (auto transfer = mRestoredDisplayTransfersAwaitingTarget.begin();
         transfer != mRestoredDisplayTransfersAwaitingTarget.end();) {
        if (transfer->InputPhysicalAddress != colorTarget) {
            ++transfer;
            continue;
        }
        transfer->AfterDrawSubmissionId = submission.Id;
        mPendingDisplayTransfers.push_back(std::move(*transfer));
        transfer = mRestoredDisplayTransfersAwaitingTarget.erase(transfer);
    }
    mCounters.EnqueueTicks += CaptureTicks() - ticks;
    if (mRuntimeProfilingEnabled) {
        mRuntimeProfile.EnqueueNanoseconds +=
            RuntimeProfileElapsedNanoseconds(phaseStart);
    }
    return true;
}

bool Oot3dNativePicaSubmissionQueue::SubmitDisplayTransfer(
    const Oot3dPicaDisplayTransfer& transfer, bool* deferredToGpu,
    std::string* error, bool* cpuCopySuppressed) {
    std::lock_guard guard(mQueueMutex);
    if (deferredToGpu != nullptr) {
        *deferredToGpu = false;
    }
    if (cpuCopySuppressed != nullptr) {
        *cpuCopySuppressed = false;
    }
    const auto inputPhysical = mMemory.TranslateGuest(transfer.InputAddress, 1U);
    Oot3dPicaTextureCopyPlan copyPlan;
    std::vector<RawCopySpan> rawCopyDestinations;
    if (transfer.TextureCopyBytes != 0U) {
        if (!BuildOot3dPicaTextureCopyPlan(
                {4U, {transfer.InputAddress, transfer.OutputAddress,
                      transfer.TextureCopyBytes, transfer.InputSize, transfer.OutputSize, transfer.Flags}},
                copyPlan, error)) return false;
        rawCopyDestinations.reserve(copyPlan.Spans.size());
        for (const auto& span : copyPlan.Spans) {
            if (!mMemory.TranslateGuest(span.InputAddress, span.Size).has_value()) {
                SetError(error, "PICA raw copy contains an unmapped range");
                return false;
            }
            // The plan works in guest addresses; copy ownership is physical.
            const auto destinationPhysical =
                mMemory.TranslateGuest(span.OutputAddress, span.Size);
            if (!destinationPhysical.has_value()) {
                SetError(error, "PICA raw copy contains an unmapped range");
                return false;
            }
            rawCopyDestinations.push_back({*destinationPhysical, span.Size});
        }
    }
    if (mDeferGpuBackedDisplayTransfers && inputPhysical.has_value() &&
        mInvalidatedColorRenderTargets.contains(*inputPhysical) && transfer.TextureCopyBytes == 0U) {
        const auto outputPhysical =
            mMemory.TranslateGuest(transfer.OutputAddress, 1U);
        if (!outputPhysical.has_value()) {
            SetError(error,
                     "restored PICA display transfer output is outside "
                     "physical memory");
            return false;
        }
        if (mNextSubmissionId == 1U) {
            SetError(error,
                     "restored PICA display transfer has no preceding draw "
                     "submission");
            return false;
        }
        // The portable CPU path below remains authoritative until a draw
        // recreates this GPU target. Allocate the original completion serial
        // now, but replay without a second guest interrupt after that draw.
        mRestoredDisplayTransfersAwaitingTarget.push_back(
            {mNextCompletionId++, 0U, *inputPhysical, *outputPhysical,
             transfer, false});
        if (cpuCopySuppressed != nullptr) {
            *cpuCopySuppressed = true;
        }
        return true;
    }
    if (!mDeferGpuBackedDisplayTransfers || !inputPhysical.has_value() ||
        (!mKnownColorRenderTargets.contains(*inputPhysical) &&
         !(transfer.TextureCopyBytes != 0U && mInvalidatedColorRenderTargets.contains(*inputPhysical)))) {
        return true;
    }
    const auto outputPhysical =
        mMemory.TranslateGuest(transfer.OutputAddress, 1U);
    if (!outputPhysical.has_value()) {
        SetError(error, "PICA display transfer output is outside physical memory");
        return false;
    }
    if (mNextSubmissionId == 1U) {
        SetError(error, "PICA display transfer has no preceding draw submission");
        return false;
    }
    const uint64_t completionId = mNextCompletionId;
    const uint64_t afterDrawSubmissionId = mNextSubmissionId - 1U;
    if (transfer.TextureCopyBytes != 0U) {
        // The source is a GPU render target, so its guest bytes are stale
        // relative to the GPU render: keeping this copy on the CPU path would
        // be a silent downgrade. Observation is required, and a destination
        // that cannot be observed is an explicit failure, never a copy left
        // owning bytes it cannot prove.
        if (!mGuestWriteRegistrar) {
            SetError(error,
                     "GPU-backed raw PICA texture copy requires guest write "
                     "observation");
            return false;
        }
        // The ordered producer promise exists before the backend executes it, so
        // a snapshot taken now already carries the identity of the version it
        // will sample.
        if (!RegisterRawCopyProducer(completionId, afterDrawSubmissionId,
                                     rawCopyDestinations, error)) {
            return false;
        }
    }
    ++mNextCompletionId;
    mPendingDisplayTransfers.push_back(
        {completionId, afterDrawSubmissionId, *inputPhysical,
         *outputPhysical, transfer});
    if (deferredToGpu != nullptr) {
        *deferredToGpu = true;
    }
    return true;
}

bool Oot3dNativePicaSubmissionQueue::SubmitMemoryFill(
    const Oot3dPicaMemoryFillCommand& command, bool* deferredToGpu,
    std::string* error) {
    std::lock_guard guard(mQueueMutex);
    if (deferredToGpu != nullptr) {
        *deferredToGpu = false;
    }
    if (!mDeferGpuBackedDisplayTransfers) {
        return true;
    }

    struct TranslatedFill {
        size_t Index = 0;
        uint32_t Start = 0;
        uint32_t End = 0;
    };
    std::vector<TranslatedFill> translated;
    for (size_t index = 0; index < command.Fills.size(); ++index) {
        const auto& fill = command.Fills[index];
        if (fill.StartAddress == 0U || (fill.Control & 1U) == 0U) {
            continue;
        }
        if (fill.EndAddress <= fill.StartAddress) {
            SetError(error, "PICA memory fill range is invalid");
            return false;
        }
        const size_t size =
            static_cast<size_t>(fill.EndAddress - fill.StartAddress);
        const auto physical = mMemory.TranslateGuest(fill.StartAddress, size);
        if (!physical.has_value()) {
            return true;
        }
        translated.push_back(
            {index, *physical, *physical + static_cast<uint32_t>(size)});
    }
    if (translated.empty()) {
        return true;
    }

    const bool hasBothBuffers = command.Fills[0].StartAddress != 0U &&
                                command.Fills[1].StartAddress != 0U;
    for (const auto& item : translated) {
        const auto& fill = command.Fills[item.Index];
        std::optional<Oot3dPicaInterruptId> interrupt;
        if (Oot3dNativeGame::MemoryFillInterruptPerUnit()) {
            interrupt = item.Index == 0U ? Oot3dPicaInterruptId::Psc0 : Oot3dPicaInterruptId::Psc1;
        } else if (item.Index == 0U && !hasBothBuffers) {
            interrupt = Oot3dPicaInterruptId::Psc0;
        } else if (item.Index == 1U) {
            interrupt = hasBothBuffers ? Oot3dPicaInterruptId::Psc0
                                       : Oot3dPicaInterruptId::Psc1;
        }
        const uint64_t completionId =
            interrupt.has_value() ? mNextCompletionId++ : 0U;
        mPendingMemoryFills.push_back(
            {completionId, mNextSubmissionId, item.Start, item.End,
             fill.Value, fill.Control, interrupt});
    }
    if (deferredToGpu != nullptr) {
        *deferredToGpu = true;
    }
    return true;
}

bool Oot3dNativePicaSubmissionQueue::SubmitInterruptAfterGpuWork(
    Oot3dPicaInterruptId interrupt, std::string* error) {
    std::lock_guard guard(mQueueMutex);
    if (mNextSubmissionId == 1U) {
        SetError(error, "PICA completion has no preceding draw submission");
        return false;
    }
    mPendingCompletions.push_back(
        {mNextCompletionId++, mNextSubmissionId - 1U, interrupt});
    return true;
}

std::span<const Oot3dPicaDrawSubmission>
Oot3dNativePicaSubmissionQueue::PendingDraws() const {
    return mPendingDraws;
}

std::vector<Oot3dPicaDrawSubmission>
Oot3dNativePicaSubmissionQueue::TakePendingDraws() {
    std::lock_guard guard(mQueueMutex);
    MaterializePendingLocked();
    std::vector<Oot3dPicaDrawSubmission> draws;
    draws.swap(mPendingDraws);
    return draws;
}

void Oot3dNativePicaSubmissionQueue::TakePendingDraws(
    std::vector<Oot3dPicaDrawSubmission>& destination) {
    std::lock_guard guard(mQueueMutex);
    MaterializePendingLocked();
    destination.clear();
    destination.swap(mPendingDraws);
}

void Oot3dNativePicaSubmissionQueue::TakePendingDraws(
    std::vector<Oot3dPicaDrawSubmission>& destination,
    std::shared_ptr<Oot3dPicaCaptureDeltaLog>& deltaLog) {
    std::lock_guard guard(mQueueMutex);
    destination.clear();
    destination.swap(mPendingDraws);
    deltaLog.reset();
    if (mDeltaLog != nullptr && mDeltaLog->Entries() != 0U) {
        // The next delta draw starts a new log with a keyframe.
        deltaLog = std::move(mDeltaLog);
        mDeltaLog = nullptr;
    }
}

std::span<const Oot3dPicaCompletionSubmission>
Oot3dNativePicaSubmissionQueue::PendingCompletions() const {
    return mPendingCompletions;
}

std::vector<Oot3dPicaCompletionSubmission>
Oot3dNativePicaSubmissionQueue::TakePendingCompletions() {
    std::lock_guard guard(mQueueMutex);
    auto completions = std::move(mPendingCompletions);
    mPendingCompletions.clear();
    return completions;
}

std::span<const Oot3dPicaDisplayTransferSubmission>
Oot3dNativePicaSubmissionQueue::PendingDisplayTransfers() const {
    return mPendingDisplayTransfers;
}

std::vector<Oot3dPicaDisplayTransferSubmission>
Oot3dNativePicaSubmissionQueue::TakePendingDisplayTransfers() {
    std::lock_guard guard(mQueueMutex);
    auto transfers = std::move(mPendingDisplayTransfers);
    mPendingDisplayTransfers.clear();
    return transfers;
}

std::span<const Oot3dPicaMemoryFillSubmission>
Oot3dNativePicaSubmissionQueue::PendingMemoryFills() const {
    return mPendingMemoryFills;
}

std::vector<Oot3dPicaMemoryFillSubmission>
Oot3dNativePicaSubmissionQueue::TakePendingMemoryFills() {
    std::lock_guard guard(mQueueMutex);
    auto fills = std::move(mPendingMemoryFills);
    mPendingMemoryFills.clear();
    return fills;
}

Oot3dPicaSubmissionBatch
Oot3dNativePicaSubmissionQueue::TakePendingBatch() {
    std::lock_guard guard(mQueueMutex);
    Oot3dPicaSubmissionBatch batch;
    MaterializePendingLocked();
    batch.Draws.swap(mPendingDraws);
    batch.Completions.swap(mPendingCompletions);
    batch.DisplayTransfers.swap(mPendingDisplayTransfers);
    batch.MemoryFills.swap(mPendingMemoryFills);
    return batch;
}

bool Oot3dNativePicaSubmissionQueue::IsQuiescent() const noexcept {
    std::lock_guard guard(mQueueMutex);
    return mPendingDraws.empty() && mPendingCompletions.empty() &&
           mPendingDisplayTransfers.empty() && mPendingMemoryFills.empty();
}

void Oot3dNativePicaSubmissionQueue::RestoreGpuColorRenderTargetOwnership(
    std::span<const uint32_t> colorPhysicalAddresses) {
    std::lock_guard guard(mQueueMutex);
    for (const uint32_t address : colorPhysicalAddresses) {
        if (address == 0U ||
            !mInvalidatedColorRenderTargets.erase(address)) {
            continue;
        }
        mKnownColorRenderTargets.insert(address);
        std::erase_if(
            mRestoredDisplayTransfersAwaitingTarget,
            [address](const auto& transfer) {
                return transfer.InputPhysicalAddress == address;
            });
    }
}

nlohmann::json Oot3dNativePicaSubmissionQueue::CaptureState() const {
    std::lock_guard guard(mQueueMutex);
    std::vector<uint32_t> knownColorRenderTargets(
        mKnownColorRenderTargets.begin(), mKnownColorRenderTargets.end());
    std::sort(knownColorRenderTargets.begin(),
              knownColorRenderTargets.end());
    std::vector<uint32_t> invalidatedColorRenderTargets(
        mInvalidatedColorRenderTargets.begin(),
        mInvalidatedColorRenderTargets.end());
    std::sort(invalidatedColorRenderTargets.begin(),
              invalidatedColorRenderTargets.end());
    nlohmann::json restoredDisplayTransfers = nlohmann::json::array();
    for (const auto &transfer : mRestoredDisplayTransfersAwaitingTarget) {
        restoredDisplayTransfers.push_back(
            {{"completion_id", transfer.CompletionId},
             {"input_physical_address", transfer.InputPhysicalAddress},
             {"output_physical_address", transfer.OutputPhysicalAddress},
             {"input_address", transfer.Transfer.InputAddress},
             {"output_address", transfer.Transfer.OutputAddress},
             {"input_size", transfer.Transfer.InputSize},
             {"output_size", transfer.Transfer.OutputSize},
             {"flags", transfer.Transfer.Flags}});
    }
    std::vector<std::pair<GeometrySnapshotKey,
                          const GeometrySnapshotCacheEntry*>>
        geometryEntries;
    geometryEntries.reserve(mGeometrySnapshotCache.size());
    for (const auto& [key, entry] : mGeometrySnapshotCache) {
        geometryEntries.emplace_back(key, &entry);
    }
    std::sort(geometryEntries.begin(), geometryEntries.end(),
              [](const auto& left, const auto& right) {
                  const auto& a = left.first;
                  const auto& b = right.first;
                  return std::tie(a.Kind, a.Slot, a.PhysicalAddress, a.Size) <
                         std::tie(b.Kind, b.Slot, b.PhysicalAddress, b.Size);
              });
    nlohmann::json geometrySnapshotCache = nlohmann::json::array();
    for (const auto& [key, entry] : geometryEntries) {
        geometrySnapshotCache.push_back(
            {{"kind", static_cast<uint8_t>(key.Kind)},
             {"slot", key.Slot},
             {"physical_address", key.PhysicalAddress},
             {"size", key.Size},
             {"content_hash", entry->ContentHashValid || entry->Bytes == nullptr
                                  ? entry->ContentHash
                                  : HashSnapshotBytes(*entry->Bytes)},
             {"content_version", entry->ContentVersion},
             {"last_used_sequence", entry->LastUsedSequence}});
    }
    std::vector<std::pair<TextureSnapshotKey,
                          const TextureSnapshotCacheEntry*>>
        textureEntries;
    textureEntries.reserve(mTextureSnapshotCache.size());
    for (const auto& [key, entry] : mTextureSnapshotCache) {
        textureEntries.emplace_back(key, &entry);
    }
    std::sort(textureEntries.begin(), textureEntries.end(),
              [](const auto& left, const auto& right) {
                  const auto& a = left.first;
                  const auto& b = right.first;
                  return std::tie(a.PhysicalAddress, a.Width, a.Height,
                                  a.Format, a.Type, a.WrapS, a.WrapT,
                                  a.MinLinear, a.MagLinear, a.MipLinear,
                                  a.LodBiasRaw, a.MinMipLevel,
                                  a.MaxMipLevel) <
                         std::tie(b.PhysicalAddress, b.Width, b.Height,
                                  b.Format, b.Type, b.WrapS, b.WrapT,
                                  b.MinLinear, b.MagLinear, b.MipLinear,
                                  b.LodBiasRaw, b.MinMipLevel,
                                  b.MaxMipLevel);
              });
    nlohmann::json textureSnapshotCache = nlohmann::json::array();
    for (const auto& [key, entry] : textureEntries) {
        textureSnapshotCache.push_back(
            {{"physical_address", key.PhysicalAddress},
             {"width", key.Width},
             {"height", key.Height},
             {"format", key.Format},
             {"type", key.Type},
             {"wrap_s", key.WrapS},
             {"wrap_t", key.WrapT},
             {"min_linear", key.MinLinear},
             {"mag_linear", key.MagLinear},
             {"mip_linear", key.MipLinear},
             {"lod_bias_raw", key.LodBiasRaw},
             {"min_mip_level", key.MinMipLevel},
             {"max_mip_level", key.MaxMipLevel},
             {"source_hash", entry->SourceHash},
             {"content_hash", entry->ContentHash}});
    }
    nlohmann::json rawCopyProducers = nlohmann::json::array();
    for (const auto& producer : mRawCopyProducers) {
        nlohmann::json spans = nlohmann::json::array();
        for (const auto& span : producer.Owned) {
            spans.push_back({{"address", span.Address},
                             {"byte_count", span.ByteCount}});
        }
        rawCopyProducers.push_back(
            {{"completion_id", producer.CompletionId},
             {"after_draw_submission_id", producer.AfterDrawSubmissionId},
             {"spans", std::move(spans)}});
    }
    return {
        {"format", "oot3d_native_pica_submission_state_v4"},
        {"defer_gpu_backed_display_transfers",
         mDeferGpuBackedDisplayTransfers},
        {"next_submission_id", mNextSubmissionId},
        {"next_completion_id", mNextCompletionId},
        {"next_geometry_content_version", mNextGeometryContentVersion},
        {"geometry_snapshot_sequence", mGeometrySnapshotSequence},
        {"geometry_snapshot_cache", std::move(geometrySnapshotCache)},
        {"texture_snapshot_cache", std::move(textureSnapshotCache)},
        {"known_color_render_targets",
         std::move(knownColorRenderTargets)},
        {"invalidated_color_render_targets",
         std::move(invalidatedColorRenderTargets)},
        {"restored_display_transfers_awaiting_target",
         std::move(restoredDisplayTransfers)},
        {"raw_copy_producers", std::move(rawCopyProducers)},
    };
}

bool Oot3dNativePicaSubmissionQueue::RestoreState(
    const nlohmann::json& state, std::string* error) {
    std::lock_guard guard(mQueueMutex);
    try {
        const std::string format = state.value("format", std::string{});
        const bool copyProducerState =
            format == "oot3d_native_pica_submission_state_v4";
        const bool extendedTextureState =
            copyProducerState ||
            format == "oot3d_native_pica_submission_state_v3";
        const bool cacheStateAvailable = extendedTextureState ||
            format == "oot3d_native_pica_submission_state_v2";
        if (!state.is_object() ||
            (!cacheStateAvailable &&
             format != "oot3d_native_pica_submission_state_v1") ||
            state.at("defer_gpu_backed_display_transfers").get<bool>() !=
                mDeferGpuBackedDisplayTransfers) {
            if (error != nullptr) {
                *error = "native PICA submission state is incompatible";
            }
            return false;
        }
        const uint64_t nextSubmissionId =
            state.at("next_submission_id").get<uint64_t>();
        const uint64_t nextCompletionId =
            state.at("next_completion_id").get<uint64_t>();
        if (nextSubmissionId == 0U || nextCompletionId == 0U) {
            if (error != nullptr) {
                *error = "native PICA submission sequence is invalid";
            }
            return false;
        }
        uint64_t nextGeometryContentVersion = 1U;
        uint64_t geometrySnapshotSequence = 0U;
        std::unordered_map<GeometrySnapshotKey,
                           GeometrySnapshotCacheEntry,
                           GeometrySnapshotKeyHash>
            geometrySnapshotCache;
        std::unordered_map<TextureSnapshotKey,
                           TextureSnapshotCacheEntry,
                           TextureSnapshotKeyHash>
            textureSnapshotCache;
        if (cacheStateAvailable) {
            nextGeometryContentVersion =
                state.at("next_geometry_content_version").get<uint64_t>();
            geometrySnapshotSequence =
                state.at("geometry_snapshot_sequence").get<uint64_t>();
            if (nextGeometryContentVersion == 0U) {
                if (error != nullptr) {
                    *error = "native PICA geometry cache sequence is invalid";
                }
                return false;
            }
            const auto& encodedGeometry =
                state.at("geometry_snapshot_cache");
            if (!encodedGeometry.is_array() ||
                encodedGeometry.size() > kGeometrySnapshotCacheLimit) {
                if (error != nullptr) {
                    *error = "native PICA geometry cache is invalid";
                }
                return false;
            }
            geometrySnapshotCache.reserve(encodedGeometry.size());
            for (const auto& encoded : encodedGeometry) {
                const uint8_t kindValue = encoded.at("kind").get<uint8_t>();
                const size_t byteCount = encoded.at("size").get<size_t>();
                GeometrySnapshotKey key{
                    static_cast<Oot3dPicaResourceKind>(kindValue),
                    encoded.at("slot").get<uint8_t>(),
                    encoded.at("physical_address").get<uint32_t>(),
                    byteCount};
                GeometrySnapshotCacheEntry entry;
                entry.ContentHash =
                    encoded.at("content_hash").get<uint64_t>();
                entry.ContentVersion =
                    encoded.at("content_version").get<uint64_t>();
                entry.LastUsedSequence =
                    encoded.at("last_used_sequence").get<uint64_t>();
                if (kindValue > static_cast<uint8_t>(
                                    Oot3dPicaResourceKind::VertexLoader) ||
                    key.Slot >= 16U || byteCount == 0U ||
                    byteCount > kMaximumSnapshotResourceBytes ||
                    entry.ContentVersion == 0U ||
                    entry.LastUsedSequence == 0U ||
                    entry.LastUsedSequence > geometrySnapshotSequence ||
                    !geometrySnapshotCache.emplace(key, std::move(entry))
                         .second) {
                    if (error != nullptr) {
                        *error = "native PICA geometry cache entry is invalid";
                    }
                    return false;
                }
            }
            const auto& encodedTextures =
                state.at("texture_snapshot_cache");
            if (!encodedTextures.is_array() ||
                encodedTextures.size() >
                    kMaximumTextureSnapshotCacheEntries) {
                if (error != nullptr) {
                    *error = "native PICA texture cache is invalid";
                }
                return false;
            }
            textureSnapshotCache.reserve(encodedTextures.size());
            for (const auto& encoded : encodedTextures) {
                TextureSnapshotKey key{
                    encoded.at("physical_address").get<uint32_t>(),
                    encoded.at("width").get<uint16_t>(),
                    encoded.at("height").get<uint16_t>(),
                    encoded.at("format").get<uint8_t>(),
                    encoded.at("type").get<uint8_t>(),
                    encoded.at("wrap_s").get<uint8_t>(),
                    encoded.at("wrap_t").get<uint8_t>(),
                    encoded.at("min_linear").get<bool>(),
                    encoded.at("mag_linear").get<bool>(),
                    extendedTextureState
                        ? encoded.at("mip_linear").get<bool>() : false,
                    extendedTextureState
                        ? encoded.at("lod_bias_raw").get<int16_t>()
                        : int16_t{0},
                    extendedTextureState
                        ? encoded.at("min_mip_level").get<uint8_t>()
                        : uint8_t{0},
                    extendedTextureState
                        ? encoded.at("max_mip_level").get<uint8_t>()
                        : uint8_t{0}};
                TextureSnapshotCacheEntry entry;
                entry.SourceHash =
                    encoded.at("source_hash").get<uint64_t>();
                entry.ContentHash =
                    encoded.at("content_hash").get<uint64_t>();
                Oot3dPicaTextureState texture;
                texture.Enabled = true;
                texture.Width = key.Width;
                texture.Height = key.Height;
                texture.PhysicalAddress = key.PhysicalAddress;
                texture.Format = key.Format;
                texture.Type = key.Type;
                texture.WrapS = key.WrapS;
                texture.WrapT = key.WrapT;
                texture.MinLinear = key.MinLinear;
                texture.MagLinear = key.MagLinear;
                texture.MipLinear = key.MipLinear;
                texture.LodBiasRaw = key.LodBiasRaw;
                texture.MinMipLevel = key.MinMipLevel;
                texture.MaxMipLevel = key.MaxMipLevel;
                const auto byteCount =
                    Oot3dPicaTextureMipChainByteSize(texture);
                if (!byteCount.has_value() ||
                    *byteCount > kMaximumSnapshotResourceBytes ||
                    !textureSnapshotCache.emplace(key, std::move(entry))
                         .second) {
                    if (error != nullptr) {
                        *error = "native PICA texture cache entry is invalid";
                    }
                    return false;
                }
            }
        }
        std::unordered_set<uint32_t> knownColorRenderTargets;
        for (const auto& encoded : state.at("known_color_render_targets")) {
            if (!knownColorRenderTargets
                     .insert(encoded.get<uint32_t>())
                     .second) {
                if (error != nullptr) {
                    *error =
                        "native PICA render target state is duplicated";
                }
                return false;
            }
        }
        std::unordered_set<uint32_t> invalidatedColorRenderTargets;
        const auto encodedInvalidated =
            state.find("invalidated_color_render_targets");
        if (encodedInvalidated != state.end()) {
            for (const auto &encoded : *encodedInvalidated) {
                if (!invalidatedColorRenderTargets
                         .insert(encoded.get<uint32_t>())
                         .second) {
                    if (error != nullptr) {
                        *error = "native PICA invalidated render target state "
                                 "is duplicated";
                    }
                    return false;
                }
            }
        }
        std::vector<RawCopyProducer> rawCopyProducers;
        if (copyProducerState) {
            const auto& encodedProducers = state.at("raw_copy_producers");
            if (!encodedProducers.is_array() ||
                encodedProducers.size() > kMaximumRawCopyProducers) {
                if (error != nullptr) {
                    *error = "native PICA raw copy ledger is invalid";
                }
                return false;
            }
            rawCopyProducers.reserve(encodedProducers.size());
            for (const auto& encoded : encodedProducers) {
                RawCopyProducer producer;
                producer.CompletionId =
                    encoded.at("completion_id").get<uint64_t>();
                producer.AfterDrawSubmissionId =
                    encoded.at("after_draw_submission_id").get<uint64_t>();
                const auto& encodedSpans = encoded.at("spans");
                if (!encodedSpans.is_array() || encodedSpans.empty() ||
                    encodedSpans.size() > kMaximumRawCopySpansPerProducer ||
                    producer.CompletionId == 0U ||
                    producer.CompletionId >= nextCompletionId ||
                    producer.AfterDrawSubmissionId >= nextSubmissionId) {
                    if (error != nullptr) {
                        *error =
                            "native PICA raw copy producer is invalid";
                    }
                    return false;
                }
                for (const auto& encodedSpan : encodedSpans) {
                    const RawCopySpan span{
                        encodedSpan.at("address").get<uint32_t>(),
                        encodedSpan.at("byte_count").get<uint32_t>()};
                    if (span.Address == 0U || span.ByteCount == 0U) {
                        if (error != nullptr) {
                            *error =
                                "native PICA raw copy span is invalid";
                        }
                        return false;
                    }
                    producer.Owned.push_back(span);
                }
                rawCopyProducers.push_back(std::move(producer));
            }
        }
        std::vector<Oot3dPicaDisplayTransferSubmission> restoredTransfers;
        const auto encodedRestoredTransfers =
            state.find("restored_display_transfers_awaiting_target");
        if (encodedRestoredTransfers != state.end()) {
            for (const auto &encoded : *encodedRestoredTransfers) {
                Oot3dPicaDisplayTransferSubmission transfer;
                transfer.CompletionId =
                    encoded.at("completion_id").get<uint64_t>();
                transfer.InputPhysicalAddress =
                    encoded.at("input_physical_address").get<uint32_t>();
                transfer.OutputPhysicalAddress =
                    encoded.at("output_physical_address").get<uint32_t>();
                transfer.Transfer = {
                    encoded.at("input_address").get<uint32_t>(),
                    encoded.at("output_address").get<uint32_t>(),
                    encoded.at("input_size").get<uint32_t>(),
                    encoded.at("output_size").get<uint32_t>(),
                    encoded.at("flags").get<uint32_t>()};
                transfer.SignalInterrupt = false;
                if (transfer.CompletionId == 0U ||
                    transfer.CompletionId >= nextCompletionId ||
                    transfer.InputPhysicalAddress == 0U ||
                    transfer.OutputPhysicalAddress == 0U) {
                    if (error != nullptr) {
                        *error = "native PICA restored display transfer state "
                                 "is invalid";
                    }
                    return false;
                }
                restoredTransfers.push_back(std::move(transfer));
            }
        }
        // Write observation is process state, not savestate state: re-arm every
        // alias of each restored producer's destination. A reference this
        // runtime cannot observe rejects the restore instead of silently
        // dropping certified ranges.
        for (const auto& producer : rawCopyProducers) {
            for (const auto& span : producer.Owned) {
                if (!ObservePhysicalWriteSpan(span.Address, span.ByteCount)) {
                    if (error != nullptr) {
                        *error =
                            "native PICA raw copy producer cannot be observed "
                            "after restore";
                    }
                    return false;
                }
            }
        }
        mHardwareWrites.clear();
        mPendingDraws.clear();
        if (mDeltaLog != nullptr) mDeltaLog->Reset();
        mPendingCompletions.clear();
        mPendingDisplayTransfers.clear();
        mPendingMemoryFills.clear();
        // Render-target images are host GPU resources and are deliberately
        // absent from a portable guest savestate. Remember their addresses as
        // invalidated: a CPU transfer remains safe immediately, and its GPU
        // replay is materialized only after a real draw recreates ownership.
        mKnownColorRenderTargets.clear();
        mInvalidatedColorRenderTargets =
            std::move(invalidatedColorRenderTargets);
        mInvalidatedColorRenderTargets.insert(
            knownColorRenderTargets.begin(), knownColorRenderTargets.end());
        mRestoredDisplayTransfersAwaitingTarget =
            std::move(restoredTransfers);
        mTextureSnapshotCache = std::move(textureSnapshotCache);
        mGeometrySnapshotCache = std::move(geometrySnapshotCache);
        ClearGeometryFront();
        ClearTextureFront();
        RebuildGeometryLru();
        ResetSnapshotObservation();
        mNextGeometryContentVersion = nextGeometryContentVersion;
        mGeometrySnapshotSequence = geometrySnapshotSequence;
        mNextSubmissionId = nextSubmissionId;
        mNextCompletionId = nextCompletionId;
        mRuntimeProfile = {};
        mRawCopyProducers = std::move(rawCopyProducers);
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error =
                std::string("native PICA submission state decode failed: ") +
                exception.what();
        }
        return false;
    }
}

} // namespace Oot3dNativeGame
