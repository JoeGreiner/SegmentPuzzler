#ifndef voxel_h
#define voxel_h

#include <iostream>
#include <vector>
#include <cstdint>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

struct Voxel {
    int x, y, z;

    // constructor for coordinates + label
    Voxel(int xIn, int yIn, int zIn) :
            x(xIn),
            y(yIn),
            z(zIn) {};

    // print voxel data
    void print();

    Voxel() : x(0), y(0), z(0) {};

    // relational comparison with another voxel
    bool operator<(const Voxel &b) const;

    // is equal comparison with another voxel
    bool operator==(const Voxel &b) const;
};

// Appends preserve voxel order and combine adjacent X coordinates.
class VoxelList {
public:
    struct Run { Voxel start; std::uint32_t length; };

    class const_iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = Voxel;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = Voxel;
        const_iterator() = default;
        explicit const_iterator(std::vector<Run>::const_iterator run) : run(run) {}
        Voxel operator*() const {
            return {static_cast<int>(std::int64_t(run->start.x) + offset), run->start.y, run->start.z};
        }
        const_iterator &operator++() {
            if (++offset == run->length) { ++run; offset = 0; }
            return *this;
        }
        const_iterator operator++(int) { auto previous = *this; ++*this; return previous; }
        bool operator==(const const_iterator &other) const { return run == other.run && offset == other.offset; }
        bool operator!=(const const_iterator &other) const { return !(*this == other); }
    private:
        std::vector<Run>::const_iterator run;
        std::uint32_t offset = 0;
    };

    VoxelList() = default;
    explicit VoxelList(const std::vector<Voxel> &voxels) { for (const auto &voxel : voxels) push_back(voxel); }
    VoxelList(const VoxelList &) = default;
    VoxelList(VoxelList &&other) noexcept { swap(other); }
    VoxelList &operator=(VoxelList other) noexcept { swap(other); return *this; }
    void swap(VoxelList &other) noexcept { runs.swap(other.runs); std::swap(voxelCount, other.voxelCount); }
    std::size_t size() const { return voxelCount; }
    bool empty() const { return voxelCount == 0; }
    std::size_t storedBytes() const { return runs.capacity() * sizeof(Run); }
    const std::vector<Run> &getRuns() const { return runs; }
    void reserveRuns(std::size_t count) { runs.reserve(count); }
    const_iterator begin() const { return const_iterator(runs.begin()); }
    const_iterator end() const { return const_iterator(runs.end()); }
    Voxel front() const { return runs.front().start; }
    void push_back(const Voxel &voxel) { appendRun(voxel, 1); }
    void emplace_back(int x, int y, int z) { push_back({x, y, z}); }
    void appendRun(const Voxel &start, std::size_t length) {
        if (length == 0) return;
        if (length > std::numeric_limits<std::uint32_t>::max() ||
            std::int64_t(start.x) + std::int64_t(length) - 1 > std::numeric_limits<int>::max() ||
            length > std::numeric_limits<std::size_t>::max() - voxelCount)
            throw std::overflow_error("Voxel run exceeds coordinate or size range");
        if (!runs.empty() && runs.back().start.y == start.y && runs.back().start.z == start.z &&
            std::int64_t(runs.back().start.x) + runs.back().length == start.x &&
            length <= std::numeric_limits<std::uint32_t>::max() - runs.back().length) {
            runs.back().length += static_cast<std::uint32_t>(length);
        } else {
            runs.push_back({start, static_cast<std::uint32_t>(length)});
        }
        voxelCount += length;
    }
private:
    std::vector<Run> runs;
    std::size_t voxelCount = 0;
};
using VoxelLists = std::vector<const VoxelList *>;

#endif /* voxel_h */
