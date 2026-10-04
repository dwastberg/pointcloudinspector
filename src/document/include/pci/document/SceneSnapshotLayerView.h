#pragma once

#include <pci/document/SceneSnapshotLayer.h>

#include <cstddef>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace pci {

// A non-owning projection over one layer alternative in a document snapshot.
// The owning SceneDocumentSnapshot must outlive the view and its iterators.
template <typename LayerSnapshot, typename LayerState>
class SceneSnapshotLayerView {
public:
    using value_type = LayerSnapshot;
    using size_type = std::size_t;
    using Projector = value_type (*)(const SceneSnapshotLayer &);

    class const_iterator {
    public:
        using value_type = LayerSnapshot;
        using difference_type = std::ptrdiff_t;
        using reference = value_type;
        using pointer = void;
        using iterator_category = std::input_iterator_tag;
        using iterator_concept = std::input_iterator_tag;

        const_iterator() = default;

        [[nodiscard]] value_type operator*() const
        {
            return projector_(layer());
        }

        const_iterator &operator++()
        {
            ++position_;
            skipUnmatchedLayers();
            return *this;
        }

        const_iterator operator++(int)
        {
            const_iterator previous = *this;
            ++*this;
            return previous;
        }

        friend bool operator==(const const_iterator &,
                               const const_iterator &) = default;

    private:
        friend class SceneSnapshotLayerView;

        const_iterator(const std::vector<SceneSnapshotLayer> *layers,
                       const std::vector<size_type> *indices,
                       const Projector projector,
                       const bool indexed,
                       const size_type position)
            : layers_(layers)
            , indices_(indices)
            , projector_(projector)
            , indexed_(indexed)
            , position_(position)
        {
            skipUnmatchedLayers();
        }

        [[nodiscard]] const SceneSnapshotLayer &layer() const
        {
            const size_type layerIndex =
                indexed_ ? (*indices_)[position_] : position_;
            return (*layers_)[layerIndex];
        }

        void skipUnmatchedLayers()
        {
            if (indexed_ || !layers_) {
                return;
            }
            while (position_ < layers_->size() &&
                   !std::holds_alternative<LayerState>(
                       (*layers_)[position_].payload)) {
                ++position_;
            }
        }

        const std::vector<SceneSnapshotLayer> *layers_ = nullptr;
        const std::vector<size_type> *indices_ = nullptr;
        Projector projector_ = nullptr;
        bool indexed_ = false;
        size_type position_ = 0;
    };

    SceneSnapshotLayerView() = default;

    SceneSnapshotLayerView(const std::vector<SceneSnapshotLayer> &layers,
                           const std::vector<size_type> &indices,
                           const Projector projector,
                           const bool indexed) noexcept
        : layers_(&layers)
        , indices_(&indices)
        , projector_(projector)
        , indexed_(indexed)
    {
    }

    [[nodiscard]] const_iterator begin() const noexcept
    {
        return {layers_, indices_, projector_, indexed_, 0};
    }

    [[nodiscard]] const_iterator end() const noexcept
    {
        return {layers_,
                indices_,
                projector_,
                indexed_,
                !layers_   ? 0
                : indexed_ ? indices_->size()
                           : layers_->size()};
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return size() == 0;
    }

    [[nodiscard]] size_type size() const noexcept
    {
        if (!layers_) {
            return 0;
        }
        if (indexed_) {
            return indices_->size();
        }
        size_type result = 0;
        for (const SceneSnapshotLayer &layer : *layers_) {
            if (std::holds_alternative<LayerState>(layer.payload)) {
                ++result;
            }
        }
        return result;
    }

    [[nodiscard]] value_type front() const
    {
        if (empty()) {
            throw std::out_of_range("snapshot layer view is empty");
        }
        return *begin();
    }

    [[nodiscard]] value_type back() const
    {
        if (empty()) {
            throw std::out_of_range("snapshot layer view is empty");
        }
        return at(size() - 1);
    }

    [[nodiscard]] value_type operator[](const size_type position) const
    {
        return valueAt(position);
    }

    [[nodiscard]] value_type at(const size_type position) const
    {
        if (position >= size()) {
            throw std::out_of_range("snapshot layer index is out of range");
        }
        return valueAt(position);
    }

private:
    [[nodiscard]] value_type valueAt(const size_type position) const
    {
        if (indexed_) {
            return projector_((*layers_)[(*indices_)[position]]);
        }
        size_type matched = 0;
        for (const SceneSnapshotLayer &layer : *layers_) {
            if (!std::holds_alternative<LayerState>(layer.payload)) {
                continue;
            }
            if (matched == position) {
                return projector_(layer);
            }
            ++matched;
        }
        throw std::out_of_range("snapshot layer index is out of range");
    }

    const std::vector<SceneSnapshotLayer> *layers_ = nullptr;
    const std::vector<size_type> *indices_ = nullptr;
    Projector projector_ = nullptr;
    bool indexed_ = false;
};

using PointCloudLayerSnapshotView =
    SceneSnapshotLayerView<PointCloudLayerSnapshot,
                           PointCloudLayerSnapshotState>;
using VectorLayerSnapshotView =
    SceneSnapshotLayerView<VectorLayerSnapshot, VectorLayerSnapshotState>;
using RasterLayerSnapshotView =
    SceneSnapshotLayerView<RasterLayerSnapshot, RasterLayerSnapshotState>;

} // namespace pci
