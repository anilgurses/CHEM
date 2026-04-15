#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "common.h"

namespace chem {

using LocationMap = std::unordered_map<std::string, NodeLocation>;
using LocationSnapshot = std::shared_ptr<const LocationMap>;

class LocationStore {
   public:
    LocationStore()
        : m_locations(std::static_pointer_cast<const LocationMap>(
              std::make_shared<LocationMap>())) {}

    void update(const std::string& id, const NodeLocation& loc) {
        auto current = std::atomic_load(&m_locations);
        auto next = std::make_shared<LocationMap>(*current);
        (*next)[id] = loc;
        auto nextConst =
            std::static_pointer_cast<const LocationMap>(std::move(next));
        std::atomic_store(&m_locations, std::move(nextConst));
    }

    LocationSnapshot snapshot() const { return std::atomic_load(&m_locations); }

    std::optional<NodeLocation> get(const std::string& id) const {
        auto snap = snapshot();
        auto it = snap->find(id);
        if (it != snap->end()) return it->second;
        return std::nullopt;
    }

   private:
    LocationSnapshot m_locations;
};

}  // namespace chem
