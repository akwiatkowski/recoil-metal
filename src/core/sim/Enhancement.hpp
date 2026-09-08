#pragma once
#include "core/sim/UnitStore.hpp"
#include "core/sim/Economy.hpp"
#include "core/sim/UnitCatalog.hpp"
#include <expected>
#include <string>
#include <string_view>

namespace rm::sim {
[[nodiscard]] Mag effectiveBuildPerTick(const UnitStore&, const UnitCatalog&, UnitIndex) noexcept;
[[nodiscard]] Mag enhancementHealthAdd(const UnitStore&, const UnitCatalog&, UnitIndex) noexcept;
[[nodiscard]] Mag enhancementRegenPerTick(const UnitStore&, const UnitCatalog&, UnitIndex) noexcept;
[[nodiscard]] bool canBuild(const UnitStore&, const UnitCatalog&, UnitIndex,
                            const unitdef::UnitDef& product);
[[nodiscard]] std::expected<void, std::string> validateEnhancementSequence(
    const UnitStore&, const UnitCatalog&, UnitId, std::span<const std::string>);
[[nodiscard]] std::expected<void, std::string> canInstallEnhancement(
    const UnitStore&, const UnitCatalog&, UnitId, std::string_view name);
/// Called only by completed, funded enhancement work. Unsupported handlers fail explicitly.
[[nodiscard]] std::expected<void, std::string> installEnhancement(
    UnitStore&, const UnitCatalog&, UnitId, std::string_view name);
/// Native counterpart of retail EnhanceTask.lua. The host must outlive bound command queues.
class EnhancementTasks final : public ScriptTaskHost {
public:
    EnhancementTasks(UnitStore& store, const UnitCatalog& catalog, std::vector<EnhancementWork>& work)
        : store_(store), catalog_(catalog), work_(work) {}
    ~EnhancementTasks() override = default;
    EnhancementTasks(const EnhancementTasks&) = delete;
    EnhancementTasks& operator=(const EnhancementTasks&) = delete;
    EnhancementTasks(EnhancementTasks&&) = delete;
    EnhancementTasks& operator=(EnhancementTasks&&) = delete;
    void onCreate(UnitId, std::string_view, std::span<const std::uint8_t>, ScriptTaskState&) override;
    std::int32_t taskTick(UnitId, std::string_view, std::span<const std::uint8_t>, ScriptTaskState&) override;
    void onDestroy(UnitId, std::string_view, std::span<const std::uint8_t>, ScriptTaskState&) override;
private:
    UnitStore& store_;
    const UnitCatalog& catalog_;
    std::vector<EnhancementWork>& work_;
};
} // namespace rm::sim
