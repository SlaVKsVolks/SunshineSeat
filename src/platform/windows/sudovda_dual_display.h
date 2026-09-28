/**
 * @file src/platform/windows/sudovda_dual_display.h
 * @brief Generation-bound ownership for a pair of provider virtual displays.
 */
#pragma once

// standard includes
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include "src/dual_display_launch_contract.h"
#include "sudovda_control.h"

namespace platf::sudovda::dual {

  constexpr std::size_t display_pair_size = 2;

  struct display_pair_request_t {
    std::uint64_t session_generation {0};
    std::string display_pair_id;
    std::array<std::string, display_pair_size> logical_display_ids;
    std::array<display_request_t, display_pair_size> display_requests;
  };

  struct display_pair_pane_t {
    std::string logical_display_id;
    std::string host_display_identity;
    std::string provider_display_name;
  };

  struct display_pair_allocation_t {
    display_pair_pane_t pane;
    // The callback is owned by the pair lease and must release exactly one
    // provider allocation. It is invoked in reverse order during teardown.
    std::function<void()> release;
  };

  using display_pair_allocate_fn = std::function<std::optional<display_pair_allocation_t>(
    std::size_t index,
    const display_request_t &request
  )>;

  [[nodiscard]] std::optional<std::string> validate_request(const display_pair_request_t &request);

  /**
   * @brief Build two persistent provider requests from server-owned identity.
   *
   * The request deliberately ignores client display IDs and host-observed
   * identities. Each provider identity is derived only from the authenticated
   * paired client, opaque pair ID, and logical pane identity.
   */
  [[nodiscard]] std::optional<display_pair_request_t> make_pair_request(
    const dual_display_launch::request_t &request,
    std::string_view authenticated_client_identity
  );

  [[nodiscard]] bool pair_identity_matches(
    const display_pair_request_t &request,
    const std::array<display_pair_pane_t, display_pair_size> &observed
  );

  /**
   * @brief Validate identities returned while the pair topology is provisional.
   *
   * Windows can temporarily report the same provider display name for both
   * newly allocated outputs until the two-output topology is committed. The
   * host-owned device identities must already be distinct, while provider-name
   * uniqueness is verified after the committed topology is re-enumerated.
   * The caller must validate the request before invoking this helper.
   */
  [[nodiscard]] inline bool provisional_pair_identity_matches(
    const display_pair_request_t &request,
    const std::array<display_pair_pane_t, display_pair_size> &observed
  ) {
    for (std::size_t index = 0; index < display_pair_size; ++index) {
      if (observed[index].logical_display_id != request.logical_display_ids[index] ||
          observed[index].host_display_identity.empty() ||
          observed[index].provider_display_name.empty()) {
        return false;
      }
    }
    return observed[0].host_display_identity != observed[1].host_display_identity;
  }

  enum class display_pair_phase_e {
    preparing,
    committed,
    closing,
    closed,
  };

  class display_pair_lease_t {
  public:
    display_pair_lease_t() = default;
    display_pair_lease_t(const display_pair_lease_t &) = delete;
    display_pair_lease_t &operator=(const display_pair_lease_t &) = delete;
    display_pair_lease_t(display_pair_lease_t &&other) noexcept;
    display_pair_lease_t &operator=(display_pair_lease_t &&other) noexcept;
    ~display_pair_lease_t();

    [[nodiscard]] static std::optional<display_pair_lease_t> acquire(
      const display_pair_request_t &request,
      const display_pair_allocate_fn &allocate,
      std::string &failure
    );

    void release() noexcept;

    [[nodiscard]] bool committed() const;
    [[nodiscard]] bool closed() const;
    [[nodiscard]] display_pair_phase_e phase() const;
    [[nodiscard]] std::uint64_t session_generation() const;
    [[nodiscard]] const std::string &display_pair_id() const;
    [[nodiscard]] const std::array<display_pair_pane_t, display_pair_size> &panes() const;
    [[nodiscard]] bool remap_provider_display_names(
      const std::array<std::string, display_pair_size> &provider_display_names
    );
    [[nodiscard]] bool remap_host_display_identities(
      const std::array<std::string, display_pair_size> &host_display_identities
    );
    [[nodiscard]] bool matches_current_identity(
      const std::array<display_pair_pane_t, display_pair_size> &observed
    ) const;

  private:
    display_pair_lease_t(
      std::uint64_t session_generation,
      std::string display_pair_id,
      std::array<std::string, display_pair_size> logical_display_ids
    );

    void move_from(display_pair_lease_t &&other) noexcept;

    std::uint64_t session_generation_ {0};
    std::string display_pair_id_;
    std::array<std::string, display_pair_size> logical_display_ids_;
    std::array<display_pair_pane_t, display_pair_size> panes_;
    std::array<std::function<void()>, display_pair_size> releases_;
    display_pair_phase_e phase_ {display_pair_phase_e::closed};
  };

  /**
   * Allocate both SudoVDA outputs as one generation-bound transaction.
   * This is intentionally the only provider-specific entry point; the
   * compositor must consume an already committed pair and never create a
   * display itself.
   */
  [[nodiscard]] std::optional<display_pair_lease_t> acquire_sudovda_pair(
    const display_pair_request_t &request,
    std::string &failure
  );

  [[nodiscard]] dual_display_launch::prepared_pair_ptr make_prepared_pair(
    const dual_display_launch::request_t &request,
    std::string_view authenticated_client_identity,
    display_pair_lease_t &&lease,
    std::string_view lease_expires_utc,
    std::string &failure
  );

  [[nodiscard]] dual_display_launch::prepared_pair_ptr prepare_sudovda_pair(
    const dual_display_launch::request_t &request,
    std::string_view authenticated_client_identity,
    std::string_view lease_expires_utc,
    std::string &failure
  );

  enum class capture_preflight_e {
    ready,
    duplication_probe_partial,
    duplication_probe_unavailable,
    identity_unavailable,
    duplicate_output,
  };

  /**
   * Classify the transient DXGI duplication probe separately from the stable
   * Windows display identity mapping. A partial duplication probe is advisory:
   * the paired capture initializer performs the authoritative two-output D3D11
   * validation immediately afterwards.
   */
  [[nodiscard]] capture_preflight_e classify_capture_preflight(
    const std::array<std::string, display_pair_size> &current_display_names,
    const std::vector<std::string> &capturable_display_names
  );

  [[nodiscard]] bool prepared_pair_is_capturable(
    const dual_display_launch::prepared_pair_t &pair,
    std::string &failure
  );

}  // namespace platf::sudovda::dual
