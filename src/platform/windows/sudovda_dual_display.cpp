/**
 * @file src/platform/windows/sudovda_dual_display.cpp
 * @brief Definitions for generation-bound SudoVDA display pairs.
 */

// standard includes
#include <algorithm>
#include <chrono>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

// local includes
#include "../../logging.h"
#include "display_reliability.h"
#include "src/display_device.h"
#include "sudovda_dual_display.h"

namespace platf::sudovda::dual {

  namespace {

    constexpr auto dual_display_allocation_retry_delay = std::chrono::milliseconds {250};

    void release_allocation(std::function<void()> &release) noexcept {
      if (!release) {
        return;
      }

      try {
        release();
      } catch (...) {
        // Provider teardown is best effort at this boundary. The pair still
        // transitions to closed so a second close cannot double-release it.
      }
      release = {};
    }

    bool non_empty(std::string_view value) {
      return !value.empty();
    }

    std::array<std::string, display_pair_size> current_display_names_for_identities(
      const std::array<std::string, display_pair_size> &device_ids
    ) {
      std::array<std::string, display_pair_size> display_names;
      const auto devices = display_device::enumerate_devices();

      for (const auto &device : devices) {
        for (std::size_t index = 0; index < display_pair_size; ++index) {
          if (display_names[index].empty() && !device_ids[index].empty() &&
              device.m_device_id == device_ids[index] && !device.m_display_name.empty()) {
            display_names[index] = device.m_display_name;
          }
        }
      }

      return display_names;
    }

    class sudovda_prepared_pair_owner_t final: public dual_display_launch::prepared_pair_owner_t {
    public:
      explicit sudovda_prepared_pair_owner_t(display_pair_lease_t &&lease):
          lease_ {std::move(lease)} {}

    private:
      display_pair_lease_t lease_;
    };

    std::string persistent_pane_identity(
      const std::string_view authenticated_client_identity,
      const std::string_view display_pair_id,
      const std::string_view logical_display_id
    ) {
      return "sunshine-dual|" + std::string {authenticated_client_identity} + "|" +
             std::string {display_pair_id} + "|" + std::string {logical_display_id};
    }

  }  // namespace

  std::optional<std::string> validate_request(const display_pair_request_t &request) {
    if (request.session_generation == 0) {
      return "invalid_session_generation";
    }
    if (!non_empty(request.display_pair_id)) {
      return "missing_display_pair_id";
    }
    if (!non_empty(request.logical_display_ids[0]) || !non_empty(request.logical_display_ids[1])) {
      return "missing_logical_display_id";
    }
    if (request.logical_display_ids[0] == request.logical_display_ids[1]) {
      return "duplicate_logical_display_id";
    }

    for (const auto &display_request : request.display_requests) {
      if (display_request.width == 0 || display_request.height == 0 || display_request.refresh_rate == 0) {
        return "invalid_display_mode";
      }
    }

    return std::nullopt;
  }

  std::optional<display_pair_request_t> make_pair_request(
    const dual_display_launch::request_t &request,
    const std::string_view authenticated_client_identity
  ) {
    if (dual_display_launch::validate_request(request) || authenticated_client_identity.empty()) {
      return std::nullopt;
    }

    display_pair_request_t pair_request;
    pair_request.session_generation = request.session_generation;
    pair_request.display_pair_id = request.display_pair_id;
    for (std::size_t index = 0; index < display_pair_size; ++index) {
      const auto &pane = request.panes[index];
      pair_request.logical_display_ids[index] = pane.logical_display_id;
      pair_request.display_requests[index] = make_persistent_request(
        pane.width,
        pane.height,
        pane.refresh_rate,
        persistent_pane_identity(
          authenticated_client_identity,
          request.display_pair_id,
          pane.logical_display_id
        )
      );
    }

    if (validate_request(pair_request)) {
      return std::nullopt;
    }
    return pair_request;
  }

  bool pair_identity_matches(
    const display_pair_request_t &request,
    const std::array<display_pair_pane_t, display_pair_size> &observed
  ) {
    if (validate_request(request)) {
      return false;
    }

    for (std::size_t index = 0; index < display_pair_size; ++index) {
      if (observed[index].logical_display_id != request.logical_display_ids[index] || !non_empty(observed[index].host_display_identity) || !non_empty(observed[index].provider_display_name)) {
        return false;
      }
    }
    return observed[0].host_display_identity != observed[1].host_display_identity &&
           observed[0].provider_display_name != observed[1].provider_display_name;
  }

  display_pair_lease_t::display_pair_lease_t(
    const std::uint64_t session_generation,
    std::string display_pair_id,
    std::array<std::string, display_pair_size> logical_display_ids
  ):
      session_generation_ {session_generation},
      display_pair_id_ {std::move(display_pair_id)},
      logical_display_ids_ {std::move(logical_display_ids)},
      phase_ {display_pair_phase_e::preparing} {}

  display_pair_lease_t::display_pair_lease_t(display_pair_lease_t &&other) noexcept {
    move_from(std::move(other));
  }

  display_pair_lease_t &display_pair_lease_t::operator=(display_pair_lease_t &&other) noexcept {
    if (this != &other) {
      release();
      move_from(std::move(other));
    }
    return *this;
  }

  display_pair_lease_t::~display_pair_lease_t() {
    release();
  }

  std::optional<display_pair_lease_t> display_pair_lease_t::acquire(
    const display_pair_request_t &request,
    const display_pair_allocate_fn &allocate,
    std::string &failure
  ) {
    failure.clear();
    if (const auto request_error = validate_request(request)) {
      failure = *request_error;
      return std::nullopt;
    }
    if (!allocate) {
      failure = "missing_display_allocator";
      return std::nullopt;
    }

    display_pair_lease_t lease {
      request.session_generation,
      request.display_pair_id,
      request.logical_display_ids
    };

    for (std::size_t index = 0; index < display_pair_size; ++index) {
      std::optional<display_pair_allocation_t> allocation;
      unsigned int completed_attempts = 0;
      do {
        failure.clear();
        allocation = allocate(index, request.display_requests[index]);
        ++completed_attempts;
        if (allocation || failure == "dual_display_pair_topology_apply_failed" ||
            !display_reliability::should_retry_sudovda_allocation(false, false, completed_attempts)) {
          break;
        }
        std::this_thread::sleep_for(dual_display_allocation_retry_delay);
      } while (true);

      if (!allocation) {
        failure = index == 0 ? "display_a_allocation_failed" : "display_b_allocation_failed";
        lease.release();
        return std::nullopt;
      }

      if (!allocation->release || allocation->pane.logical_display_id != request.logical_display_ids[index] || !non_empty(allocation->pane.host_display_identity) || !non_empty(allocation->pane.provider_display_name)) {
        release_allocation(allocation->release);
        failure = "display_identity_verification_failed";
        lease.release();
        return std::nullopt;
      }

      lease.panes_[index] = std::move(allocation->pane);
      lease.releases_[index] = std::move(allocation->release);
    }

    if (!provisional_pair_identity_matches(request, lease.panes_)) {
      failure = "display_pair_identity_mismatch";
      lease.release();
      return std::nullopt;
    }

    lease.phase_ = display_pair_phase_e::committed;
    return lease;
  }

  void display_pair_lease_t::release() noexcept {
    if (phase_ == display_pair_phase_e::closed) {
      return;
    }

    phase_ = display_pair_phase_e::closing;
    for (std::size_t index = display_pair_size; index-- > 0;) {
      release_allocation(releases_[index]);
    }
    phase_ = display_pair_phase_e::closed;
  }

  bool display_pair_lease_t::committed() const {
    return phase_ == display_pair_phase_e::committed;
  }

  bool display_pair_lease_t::closed() const {
    return phase_ == display_pair_phase_e::closed;
  }

  display_pair_phase_e display_pair_lease_t::phase() const {
    return phase_;
  }

  std::uint64_t display_pair_lease_t::session_generation() const {
    return session_generation_;
  }

  const std::string &display_pair_lease_t::display_pair_id() const {
    return display_pair_id_;
  }

  const std::array<display_pair_pane_t, display_pair_size> &display_pair_lease_t::panes() const {
    return panes_;
  }

  bool display_pair_lease_t::remap_provider_display_names(
    const std::array<std::string, display_pair_size> &provider_display_names
  ) {
    if (!committed() || provider_display_names[0].empty() ||
        provider_display_names[1].empty() ||
        provider_display_names[0] == provider_display_names[1]) {
      return false;
    }

    for (std::size_t index = 0; index < display_pair_size; ++index) {
      panes_[index].provider_display_name = provider_display_names[index];
    }
    return true;
  }

  bool display_pair_lease_t::remap_host_display_identities(
    const std::array<std::string, display_pair_size> &host_display_identities
  ) {
    if (!committed() || host_display_identities[0].empty() ||
        host_display_identities[1].empty() ||
        host_display_identities[0] == host_display_identities[1]) {
      return false;
    }

    for (std::size_t index = 0; index < display_pair_size; ++index) {
      panes_[index].host_display_identity = host_display_identities[index];
    }
    return true;
  }

  bool display_pair_lease_t::matches_current_identity(
    const std::array<display_pair_pane_t, display_pair_size> &observed
  ) const {
    if (!committed()) {
      return false;
    }

    for (std::size_t index = 0; index < display_pair_size; ++index) {
      if (observed[index].logical_display_id != logical_display_ids_[index] || observed[index].host_display_identity != panes_[index].host_display_identity || observed[index].provider_display_name != panes_[index].provider_display_name || observed[index].host_display_identity.empty() || observed[index].provider_display_name.empty()) {
        return false;
      }
    }
    return observed[0].host_display_identity != observed[1].host_display_identity &&
           observed[0].provider_display_name != observed[1].provider_display_name;
  }

  void display_pair_lease_t::move_from(display_pair_lease_t &&other) noexcept {
    session_generation_ = other.session_generation_;
    display_pair_id_ = std::move(other.display_pair_id_);
    logical_display_ids_ = std::move(other.logical_display_ids_);
    panes_ = std::move(other.panes_);
    releases_ = std::move(other.releases_);
    phase_ = other.phase_;
    other.phase_ = display_pair_phase_e::closed;
    other.releases_ = {};
  }

  std::optional<display_pair_lease_t> acquire_sudovda_pair(
    const display_pair_request_t &request,
    std::string &failure
  ) {
    for (unsigned int pair_attempt = 1;
         pair_attempt <= display_reliability::sudovda_pair_transaction_attempts();
         ++pair_attempt) {
      std::array<std::shared_ptr<virtual_display_allocation_t>, display_pair_size> allocations;
      std::array<std::string, display_pair_size> device_ids;
      std::array<std::string, display_pair_size> display_names;
      std::array<display_request_t, display_pair_size> display_requests {};
      bool topology_apply_failed = false;
      failure.clear();

      auto lease = display_pair_lease_t::acquire(
        request,
        [&](const std::size_t index, const display_request_t &display_request) -> std::optional<display_pair_allocation_t> {
          // A pair must be discovered before either output changes the desktop
          // topology. The single-display allocator's normal mode is intentionally
          // deferred here: it would hide the physical outputs after pane A and
          // make pane B resolve against pane A's topology.
          auto result = create_virtual_display_allocation(
            display_request,
            false,
            false,
            index == 0,
            false
          );
          if (!result.allocation || !result.result.display_name) {
            return std::nullopt;
          }

          if (!result.result.device_id || result.result.device_id->empty()) {
            result.allocation.reset();
            return std::nullopt;
          }

          allocations[index] = std::move(result.allocation);
          device_ids[index] = *result.result.device_id;
          display_names[index] = *result.result.display_name;
          display_requests[index] = display_request;

          if (index == display_pair_size - 1) {
            std::vector<stream_display_t> displays;
            displays.reserve(display_pair_size);
            for (std::size_t pane = 0; pane < display_pair_size; ++pane) {
              displays.push_back(stream_display_t {device_ids[pane], display_names[pane], display_requests[pane]});
            }

            const auto topology_result = apply_sudovda_stream_topology(displays, "dual_pair_allocation");
            if (!topology_result.applied || topology_result.device_ids.size() != display_pair_size) {
              topology_apply_failed = true;
              allocations[index].reset();
              failure = "dual_display_pair_topology_apply_failed";
              return std::nullopt;
            }
            std::copy_n(topology_result.device_ids.begin(), display_pair_size, device_ids.begin());
          }

          const auto host_identity = device_ids[index];
          const auto provider_display_name = *result.result.display_name;
          return display_pair_allocation_t {
            display_pair_pane_t {
              request.logical_display_ids[index],
              host_identity,
              provider_display_name
            },
            [owned_allocation = allocations[index]]() mutable {
              owned_allocation.reset();
            }
          };
        },
        failure
      );

      if (lease) {
        if (!lease->remap_host_display_identities(device_ids)) {
          failure = "display_pair_identity_mismatch";
          lease->release();
          return std::nullopt;
        }

        constexpr auto provider_name_remap_window = std::chrono::seconds {5};
        constexpr auto provider_name_remap_interval = std::chrono::milliseconds {100};
        const auto provider_name_remap_deadline =
          std::chrono::steady_clock::now() + provider_name_remap_window;

        do {
          const auto current_display_names = current_display_names_for_identities(device_ids);
          if (lease->remap_provider_display_names(current_display_names)) {
            for (std::size_t index = 0; index < display_pair_size; ++index) {
              BOOST_LOG(info) << "dual_display_provider_name_committed: pane=" << index
                              << " identity=" << device_ids[index]
                              << " provider=" << current_display_names[index];
            }
            return lease;
          }
          std::this_thread::sleep_for(provider_name_remap_interval);
        } while (std::chrono::steady_clock::now() < provider_name_remap_deadline);

        failure = "display_pair_identity_mismatch";
        lease->release();
        return std::nullopt;
      }

      if (topology_apply_failed) {
        failure = "dual_display_pair_topology_apply_failed";
      }
      if (!display_reliability::should_retry_sudovda_pair_transaction(
            false,
            topology_apply_failed,
            pair_attempt
          )) {
        break;
      }

      BOOST_LOG(warning) << "SunshineSeat SudoVDA restarting complete pair transaction after topology apply failure [attempt="
                        << pair_attempt << '/'
                        << display_reliability::sudovda_pair_transaction_attempts() << ']';
      std::this_thread::sleep_for(dual_display_allocation_retry_delay);
    }

    return std::nullopt;
  }

  dual_display_launch::prepared_pair_ptr make_prepared_pair(
    const dual_display_launch::request_t &request,
    const std::string_view authenticated_client_identity,
    display_pair_lease_t &&lease,
    const std::string_view lease_expires_utc,
    std::string &failure
  ) {
    failure.clear();
    if (dual_display_launch::validate_request(request)) {
      failure = "invalid_dual_display_request";
      return {};
    }
    if (authenticated_client_identity.empty()) {
      failure = "dual_display_authenticated_client_identity_unavailable";
      return {};
    }
    if (!lease.committed() || lease.session_generation() != request.session_generation || lease.display_pair_id() != request.display_pair_id) {
      failure = "prepared_pair_lease_mismatch";
      return {};
    }

    const auto &lease_panes = lease.panes();
    std::array<dual_display_launch::observed_pane_t, display_pair_size> observed;
    for (std::size_t index = 0; index < display_pair_size; ++index) {
      observed[index] = {
        lease_panes[index].logical_display_id,
        lease_panes[index].host_display_identity,
      };
    }

    auto manifest = dual_display_launch::make_manifest(request, observed, lease_expires_utc, failure);
    if (!manifest) {
      return {};
    }

    auto pair = std::make_shared<dual_display_launch::prepared_pair_t>();
    pair->manifest = std::move(*manifest);
    pair->owner_identity = std::string {authenticated_client_identity};
    for (std::size_t index = 0; index < display_pair_size; ++index) {
      pair->provider_display_names[index] = lease_panes[index].provider_display_name;
    }
    pair->owner = std::make_shared<sudovda_prepared_pair_owner_t>(std::move(lease));

    if (const auto pair_failure = dual_display_launch::validate_prepared_pair(*pair)) {
      failure = *pair_failure;
      return {};
    }
    return pair;
  }

  dual_display_launch::prepared_pair_ptr prepare_sudovda_pair(
    const dual_display_launch::request_t &request,
    const std::string_view authenticated_client_identity,
    const std::string_view lease_expires_utc,
    std::string &failure
  ) {
    failure.clear();
    const auto pair_request = make_pair_request(request, authenticated_client_identity);
    if (!pair_request) {
      failure = authenticated_client_identity.empty() ? "dual_display_authenticated_client_identity_unavailable" : "invalid_dual_display_request";
      return {};
    }

    auto lease = acquire_sudovda_pair(*pair_request, failure);
    if (!lease) {
      return {};
    }
    return make_prepared_pair(
      request,
      authenticated_client_identity,
      std::move(*lease),
      lease_expires_utc,
      failure
    );
  }

  capture_preflight_e classify_capture_preflight(
    const std::array<std::string, display_pair_size> &current_display_names,
    const std::vector<std::string> &capturable_display_names
  ) {
    if (std::any_of(current_display_names.begin(), current_display_names.end(), [](const auto &name) {
          return name.empty();
        })) {
      return capture_preflight_e::identity_unavailable;
    }
    if (current_display_names[0] == current_display_names[1]) {
      return capture_preflight_e::duplicate_output;
    }

    const auto ready_duplication_probe_count = std::count_if(
      current_display_names.begin(),
      current_display_names.end(),
      [&](const auto &name) {
        return std::find(capturable_display_names.begin(), capturable_display_names.end(), name) !=
               capturable_display_names.end();
      }
    );
    if (ready_duplication_probe_count == display_pair_size) {
      return capture_preflight_e::ready;
    }
    if (ready_duplication_probe_count == 1) {
      return capture_preflight_e::duplication_probe_partial;
    }
    return capture_preflight_e::duplication_probe_unavailable;
  }

  bool prepared_pair_is_capturable(
    const dual_display_launch::prepared_pair_t &pair,
    std::string &failure
  ) {
    failure.clear();
    if (const auto pair_failure = dual_display_launch::validate_prepared_pair(pair)) {
      failure = *pair_failure;
      return false;
    }

    constexpr auto capturability_probe_window = std::chrono::seconds {5};
    constexpr auto capturability_probe_interval = std::chrono::milliseconds {100};
    const auto deadline = std::chrono::steady_clock::now() + capturability_probe_window;
    std::vector<std::string> display_names;
    std::array<std::string, display_pair_size> current_display_names;
    std::array<std::string, display_pair_size> capture_names;
    std::array<std::string, display_pair_size> host_display_identities;

    for (std::size_t index = 0; index < display_pair_size; ++index) {
      host_display_identities[index] = pair.manifest.panes[index].host_display_identity;
    }

    for (;;) {
      display_names = capturable_display_names(false);
      current_display_names = current_display_names_for_identities(host_display_identities);

      for (std::size_t index = 0; index < display_pair_size; ++index) {
        const auto &provider_display_name = pair.provider_display_names[index];
        capture_names[index] = current_display_names[index].empty() ? provider_display_name : current_display_names[index];
      }

      const auto preflight = classify_capture_preflight(current_display_names, display_names);
      if (preflight == capture_preflight_e::ready) {
        for (std::size_t index = 0; index < display_pair_size; ++index) {
          if (capture_names[index] != pair.provider_display_names[index]) {
            BOOST_LOG(info) << "dual_display_provider_name_remapped: pane=" << index
                            << " provider=" << pair.provider_display_names[index]
                            << " current=" << capture_names[index];
          }
        }
        return true;
      }

      if (std::chrono::steady_clock::now() >= deadline) {
        break;
      }
      std::this_thread::sleep_for(capturability_probe_interval);
    }

    std::ostringstream available_names;
    for (std::size_t index = 0; index < display_names.size(); ++index) {
      if (index != 0) {
        available_names << ',';
      }
      available_names << display_names[index];
    }
    for (std::size_t index = 0; index < display_pair_size; ++index) {
      BOOST_LOG(warning) << "dual_display_capturability_probe_partial: pane=" << index
                         << " provider=" << pair.provider_display_names[index]
                         << " identity=" << pair.manifest.panes[index].host_display_identity
                         << " current=" << current_display_names[index]
                         << " capture=" << capture_names[index]
                         << " available=" << available_names.str();
    }

    const auto final_preflight = classify_capture_preflight(current_display_names, display_names);
    if (final_preflight == capture_preflight_e::duplication_probe_partial) {
      BOOST_LOG(warning) << "dual_display_duplication_probe_partial: proceeding to authoritative paired D3D11 capture initialization";
      return true;
    }
    if (final_preflight == capture_preflight_e::duplicate_output) {
      failure = "dual_display_capture_name_duplicate";
    } else if (final_preflight == capture_preflight_e::identity_unavailable) {
      failure = "dual_display_capture_identity_unavailable";
    } else {
      failure = "dual_display_capture_not_enumerated";
    }
    return false;
  }

}  // namespace platf::sudovda::dual
