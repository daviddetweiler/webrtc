#include <api/create_peerconnection_factory.h>
#include <api/peer_connection_interface.h>
#include <rtc_base/thread.h>

#include <exception>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>

#define BOOST_ALL_NO_LIB
#include <boost/json.hpp>
#include <boost/json/src.hpp>

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/jsep.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "websocketpp/config/asio_no_tls.hpp"
#include "websocketpp/server.hpp"

using server_type = websocketpp::server<websocketpp::config::asio>;
using message_ptr = server_type::message_ptr;

using peercon_ptr = rtc::scoped_refptr<webrtc::PeerConnectionInterface>;

namespace receiver {
enum class level { error, warning, info };

constexpr auto name(level l) {
  switch (l) {
    case level::error:
      return "error";

    case level::warning:
      return "warning";

    case level::info:
      return "info";
  }
}

std::mutex log_lock{};
std::ofstream log_file{"relay.log"};

template <typename... types>
void log_to(std::ostream& stream, level severity, types&&... args) {
  stream << "[relay:" << name(severity) << "]";
  ((stream << ' ' << std::forward<types>(args)), ...);
  stream << std::endl;
}

template <typename... types>
void log(level severity, types&&... args) {
  std::lock_guard guard{log_lock};
  log_to(std::cerr, severity, std::forward<types>(args)...);
  log_to(log_file, severity, std::forward<types>(args)...);
}

struct webrtc_factory {
  std::unique_ptr<rtc::Thread> signal_thread;
  rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory{};

  webrtc_factory()
      : signal_thread{rtc::Thread::CreateWithSocketServer()}, factory{} {
    signal_thread->Start();
    factory = webrtc::CreatePeerConnectionFactory(
        nullptr, nullptr, signal_thread.get(), nullptr,
        webrtc::CreateBuiltinAudioEncoderFactory(),
        webrtc::CreateBuiltinAudioDecoderFactory(),
        webrtc::CreateBuiltinVideoEncoderFactory(),
        webrtc::CreateBuiltinVideoDecoderFactory(), nullptr, nullptr);

    if (!factory)
      throw std::runtime_error{"Failed to create PeerConnectionFactory"};
  }

  auto operator->() { return factory.operator->(); }
};

template <typename derived>
class socket_server {
 public:
  void start(unsigned port) {
    server.init_asio();
    server.set_reuse_addr(true);
    server.set_message_handler([this](auto&&... args) {
      log(level::info, "message received");
      self().on_message(std::forward<decltype(args)>(args)...);
    });

    server.set_open_handler([this](auto&&... args) {
      log(level::info, "socket opened");
      self().on_open(std::forward<decltype(args)>(args)...);
    });

    server.set_close_handler([this](auto&&... args) {
      log(level::info, "socket closed");
      self().on_close(std::forward<decltype(args)>(args)...);
    });

    server.listen(port);
    server.start_accept();
    server_thread = std::thread{[this] { server.run(); }};
  }

  void shut_down() {
    server.stop_listening();
    // self().close_all();
    server.stop();
    server_thread.join();
  }

 protected:
  server_type server{};

 private:
  std::thread server_thread{};

  auto& self() { return *reinterpret_cast<derived*>(this); }

  template <typename... types>
  void on_open(types&&...) {}

  template <typename... types>
  void on_message(types&&...) {}

  template <typename... types>
  void on_close(types&&...) {}

  void close_all() {}
};

using track_callback =
    std::function<void(rtc::scoped_refptr<webrtc::RtpTransceiverInterface>)>;

class local_desc_observer
    : public webrtc::SetLocalDescriptionObserverInterface {
 public:
  template <typename... types>
  static auto make(types&&... args) {
    return rtc::make_ref_counted<local_desc_observer>(
        std::forward<types>(args)...);
  }

  local_desc_observer(peercon_ptr peer, server_type::connection_ptr socket)
      : peer{peer}, socket{socket} {}

  void OnSetLocalDescriptionComplete(webrtc::RTCError error) override {
    if (!error.ok()) {
      log(level::error, reinterpret_cast<std::uintptr_t>(this),
          "SetLocalDescription failed:", error.message());

      return;
    }

    log(level::info, reinterpret_cast<std::uintptr_t>(this),
        "SetLocalDescription succeeded");

    const auto desc = peer->local_description();
    boost::json::object data{};
    data["type"] = webrtc::SdpTypeToString(desc->GetType());
    std::string sdp{};
    if (!desc->ToString(&sdp)) {
      log(level::error, reinterpret_cast<std::uintptr_t>(this),
          "failed to serialize SDP");

      return;
    }

    data["sdp"] = sdp;
    boost::json::object msg{};
    msg["description"] = data;
    socket->send(boost::json::serialize(msg), websocketpp::frame::opcode::text);
  }

 private:
  peercon_ptr peer;
  server_type::connection_ptr socket;
};

class remote_desc_observer
    : public webrtc::SetRemoteDescriptionObserverInterface {
 public:
  template <typename... types>
  static auto make(types&&... args) {
    return rtc::make_ref_counted<remote_desc_observer>(
        std::forward<types>(args)...);
  }

  remote_desc_observer(
      peercon_ptr peer,
      rtc::scoped_refptr<webrtc::SetLocalDescriptionObserverInterface> observer,
      bool is_offer)
      : peer{peer}, observer{observer}, is_offer{is_offer} {}

  void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
    if (is_offer)
      peer->SetLocalDescription(observer);
  }

 private:
  peercon_ptr peer;
  rtc::scoped_refptr<webrtc::SetLocalDescriptionObserverInterface> observer;
  bool is_offer;
};

// TODO: stop the relayed stream before destruction completes, to avoid a racy
// segfault
// TODO: implement renegotiation so the source can be switched out?
class webrtc_observer : public webrtc::PeerConnectionObserver {
 public:
  webrtc_observer(webrtc_factory& factory,
                  server_type::connection_ptr signal_socket,
                  track_callback on_track)
      : factory{factory},
        peer{},
        signal_socket{signal_socket},
        on_track{on_track},
        current_sender{},
        ignore_offer{},
        making_offer{} {
    peer = create_peer(this);
    signal_socket->set_message_handler(
        [this](websocketpp::connection_hdl hdl,
               server_type::message_ptr message) { on_message(hdl, message); });
  }

  ~webrtc_observer() { close(); }

  template <typename... types>
  static auto make(types&&... args) {
    return rtc::make_ref_counted<webrtc_observer>(std::forward<types>(args)...);
  }

  void switch_track(
      rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
    if (current_sender) {
      log(level::info, "removing existing track sender");
      const auto maybe_removed = peer->RemoveTrackOrError(current_sender);
      if (!maybe_removed.ok()) {
        log(level::error, "failed to remove existing track from peer:",
            maybe_removed.message());

        return;
      }
    }

    const auto real_track = transceiver->receiver()->track();
    const auto sender = peer->AddTrack(real_track, {"mirrored_stream"});
    if (!sender.ok())
      log(level::error, "failed to add track:", sender.error().message());
    else
      log(level::info, "added track to peer");
  }

 private:
  static constexpr auto polite = false;

  webrtc_factory& factory;
  rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer;
  server_type::connection_ptr signal_socket;
  track_callback on_track;
  rtc::scoped_refptr<webrtc::RtpSenderInterface> current_sender;

  bool ignore_offer;
  bool making_offer;

  void close() {
    peer->Close();
    log(level::info, reinterpret_cast<std::uintptr_t>(this), "closing peer");
  }

  static rtc::scoped_refptr<webrtc::PeerConnectionInterface> create_peer(
      webrtc_observer* host) {
    webrtc::PeerConnectionInterface::RTCConfiguration config{};
    config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
    webrtc::PeerConnectionInterface::IceServer turner{};
    turner.uri = "turn:54.200.166.206:3478?transport=tcp";
    turner.username = "user";
    turner.password = "root";
    config.servers.emplace_back(std::move(turner));

    const auto maybe_pc = host->factory->CreatePeerConnectionOrError(
        config, webrtc::PeerConnectionDependencies{host});

    if (!maybe_pc.ok()) {
      throw std::runtime_error{"failed to create PeerConnection"};
    }

    log(level::info, "created PeerConnection\n");

    return maybe_pc.value();
  }

  void on_message(websocketpp::connection_hdl hdl,
                  server_type::message_ptr message) {
    // TODO: we really should check that it's the expected hdl here
    const auto opcode = message->get_opcode();
    if (opcode != 1) {
      log(level::warning, reinterpret_cast<std::uintptr_t>(this),
          "I don't know how to use this frame", opcode);

      return;
    }

    auto payload = boost::json::parse(message->get_payload()).as_object();
    if (payload.contains("description")) {
      auto offer = payload["description"].as_object();
      auto type = offer["type"].as_string();

      const auto offer_collision =
          type == "offer" &&
          (making_offer ||
           peer->signaling_state() != webrtc::PeerConnectionInterface::kStable);

      ignore_offer = !polite && offer_collision;
      if (ignore_offer)
        return;

      auto description = webrtc::CreateSessionDescription(
          webrtc::SdpTypeFromString(type.c_str()).value(),
          offer["sdp"].as_string().c_str());

      const auto is_offer = type == "offer";
      peer->SetRemoteDescription(
          std::move(description),
          remote_desc_observer::make(
              peer, local_desc_observer::make(peer, signal_socket), is_offer));

    } else if (payload.contains("candidate")) {
      auto blob = payload["candidate"].as_object();
      webrtc::SdpParseError error{};
      std::unique_ptr<webrtc::IceCandidateInterface> candidate{
          webrtc::CreateIceCandidate(blob["sdpMid"].as_string().c_str(),
                                     blob["sdpMLineIndex"].as_int64(),
                                     blob["candidate"].as_string().c_str(),
                                     &error)};

      if (!candidate) {
        log(level::error, reinterpret_cast<std::uintptr_t>(this),
            "failed to parse ICE candidate: ", error.description);
        return;
      }

      peer->AddIceCandidate(
          std::move(candidate), [this](webrtc::RTCError error) {
            if (!error.ok())
              log(level::error, reinterpret_cast<std::uintptr_t>(this),
                  "failed to set ICE candidate with error:", error.message());
          });
    }
  }

  void OnNegotiationNeededEvent(uint32_t id) override {
    factory.signal_thread->PostTask([this, id] {
      if (peer->ShouldFireNegotiationNeededEvent(id)) {
        making_offer = true;
        peer->SetLocalDescription(
            local_desc_observer::make(peer, signal_socket));

        making_offer = false;
      }
    });
  }

  void OnSignalingChange(
      webrtc::PeerConnectionInterface::SignalingState new_state) override {
    const auto name = [new_state] {
      switch (new_state) {
        case decltype(new_state)::kStable:
          return "kStable";

        case decltype(new_state)::kHaveLocalOffer:
          return "kHaveLocalOffer";

        case decltype(new_state)::kHaveLocalPrAnswer:
          return "kHaveLocalPrAnswer";

        case decltype(new_state)::kHaveRemoteOffer:
          return "kHaveRemoteOffer";

        case decltype(new_state)::kHaveRemotePrAnswer:
          return "kHaveRemotePrAnswer";

        case decltype(new_state)::kClosed:
          return "kClosed";
      }
    }();

    log(level::info, reinterpret_cast<std::uintptr_t>(this),
        "Signaling state change:", name);
  }

  void OnDataChannel(
      rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override {
    log(level::info, reinterpret_cast<std::uintptr_t>(this),
        "Added data channel to peer");
  }

  void OnIceGatheringChange(
      webrtc::PeerConnectionInterface::IceGatheringState state) override {
    log(level::info, reinterpret_cast<std::uintptr_t>(this),
        "ICE gathering state change:", [state] {
          switch (state) {
            case decltype(state)::kIceGatheringComplete:
              return "Complete";

            case decltype(state)::kIceGatheringGathering:
              return "Gathering";

            case decltype(state)::kIceGatheringNew:
              return "New";
          }
        }());
  }

  void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
    std::string blob{};
    if (!candidate->ToString(&blob)) {
      log(level::error, reinterpret_cast<std::uintptr_t>(this),
          "failed to serialize ICE candidate");
      return;
    }

    boost::json::object data{};
    boost::json::object inner_blob{};
    inner_blob["candidate"] = blob;
    inner_blob["sdpMid"] = candidate->sdp_mid();
    inner_blob["sdpMLineIndex"] = candidate->sdp_mline_index();
    data["candidate"] = inner_blob;
    signal_socket->send(boost::json::serialize(data),
                        websocketpp::frame::opcode::text);
  }

  void OnTrack(rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver)
      override {
    on_track(transceiver);
  }
};

using peer_ptr =
    rtc::scoped_refptr<rtc::FinalRefCountedObject<webrtc_observer>>;

class sink_server : public socket_server<sink_server> {
 public:
  sink_server(webrtc_factory& factory)
      : socket_server{},
        factory{factory},
        connections{},
        track_lock{},
        transceiver{} {}

  template <typename... types>
  void on_open(websocketpp::connection_hdl hdl, types&&...) {
    log(level::info, "New sink has appeared");
    const auto new_connection = server.get_con_from_hdl(hdl);
    const auto maybe = connections.find(new_connection);
    if (maybe == connections.end()) {
      const auto peer =
          webrtc_observer::make(factory, new_connection, [](auto&&...) {});

      if (transceiver)
        peer->switch_track(transceiver);

      connections[new_connection] = peer;
    }
  }

  void on_close(websocketpp::connection_hdl hdl) {
    const auto new_connection = server.get_con_from_hdl(hdl);
    auto maybe = connections.find(new_connection);
    if (maybe != connections.end()) {
      log(level::warning, "source disconnected");
      auto& [connection, peer] = *maybe;
      peer = nullptr;
      connections.erase(maybe);
    }
  }

  template <typename... types>
  void on_message(types&&...) {}

  void close_all() {
    log(level::info, "closing source connections");
    for (auto& [connection, peer] : connections) {
      connection->close(websocketpp::close::status::going_away,
                        "Server shutting down");

      peer = nullptr;
    }

    connections.clear();
  }

  void switch_source(
      rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
    log(level::info, "switching sources");
    this->transceiver = transceiver;
    // TODO: OnNegotiationNeeded?
    for (const auto& [conn, peer] : connections)
      peer->switch_track(transceiver);
  }

 private:
  webrtc_factory& factory;
  std::map<decltype(server)::connection_ptr, peer_ptr> connections{};

  std::mutex track_lock{};
  rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver{};
};

class source_server : public socket_server<source_server> {
 public:
  source_server(webrtc_factory& factory, sink_server& sink)
      : socket_server{}, sink{sink}, connection{}, factory{factory}, peer{} {}

  template <typename... types>
  void on_open(websocketpp::connection_hdl hdl, types&&...) {
    const auto new_connection = server.get_con_from_hdl(hdl);
    if (connection) {
      log(level::warning, "rejecting sink connection; one already exists");
      return;
    }

    connection = new_connection;
    peer = webrtc_observer::make(
        factory, connection,
        [this](rtc::scoped_refptr<webrtc::RtpTransceiverInterface> track) {
          on_track(track);
        });
  }

  void on_close(websocketpp::connection_hdl hdl) {
    const auto new_connection = server.get_con_from_hdl(hdl);
    if (connection == new_connection) {
      log(level::warning, "source disconnected");
      connection = nullptr;
      peer = nullptr;
    }
  }

  template <typename... types>
  void on_message(types&&...) {}

  void on_track(rtc::scoped_refptr<webrtc::RtpTransceiverInterface> track) {
    log(level::info, "track added",
        reinterpret_cast<std::uintptr_t>(track.get()));

    if (track->receiver()->track()->enabled())
      log(level::info, "track enabled",
          reinterpret_cast<std::uintptr_t>(track.get()));

    sink.switch_source(track);
  }

  void close_all() {
    if (connection) {
      log(level::info, "closing source connection");
      connection->close(websocketpp::close::status::going_away,
                        "Server shutting down");

      peer = nullptr;
    }
  }

 private:
  sink_server& sink;
  decltype(server)::connection_ptr connection;
  webrtc_factory& factory;
  peer_ptr peer;
};

template <typename type>
class scoped_session {
 public:
  scoped_session(type& server, unsigned port) : server{server} {
    server.start(port);
  }

  ~scoped_session() { server.shut_down(); }

 private:
  type& server;
};
}  // namespace receiver

int main() {
  using namespace receiver;

  // For reasons I genuinely cannot explain, it is *imperative* that all WebRTC
  // objects in this process share the same factory and signal thread
  webrtc_factory factory{};
  try {
    rtc::LogMessage::LogToDebug(rtc::LS_ERROR);
    sink_server sink{factory};
    source_server source{factory, sink};
    scoped_session source_session{source, 9002};
    scoped_session sink_session{sink, 9003};

    std::string input{};
    while (std::cin >> input) {
      if (input == "exit")
        break;
    }
  } catch (const std::exception& error) {
    log(level::error, "fatal error: ", error.what());
    return -1;
  }
}
