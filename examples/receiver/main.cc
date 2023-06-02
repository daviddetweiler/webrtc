#include <api/create_peerconnection_factory.h>
#include <api/peer_connection_interface.h>
#include <rtc_base/thread.h>

#define BOOST_ALL_NO_LIB
#include <boost/json.hpp>
#include <boost/json/src.hpp>
#include <iostream>

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/jsep.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "websocketpp/config/asio_no_tls.hpp"
#include "websocketpp/server.hpp"

using server_type = websocketpp::server<websocketpp::config::asio>;
using message_ptr = server_type::message_ptr;
using websocketpp::lib::bind;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;

namespace receiver {
template <typename consumer_type>
class observer : public webrtc::PeerConnectionObserver,
                 public webrtc::SetSessionDescriptionObserver,
                 public webrtc::CreateSessionDescriptionObserver {
 public:
  observer(consumer_type& consumer, std::mutex& exit_lock)
      : server{},
        peer_connection{},
        track{},
        connection{},
        waiter_thread{[&exit_lock, this] {
          exit_lock.lock();
          server.stop_listening();
          close();
          server.stop();
          exit_lock.unlock();
        }},
        consumer{consumer} {}

  void start_signal_server() {
    const auto signaling_thread = rtc::Thread::CreateWithSocketServer();
    signaling_thread->Start();
    const auto pc_factory = webrtc::CreatePeerConnectionFactory(
        nullptr, nullptr, signaling_thread.get(), nullptr,
        webrtc::CreateBuiltinAudioEncoderFactory(),
        webrtc::CreateBuiltinAudioDecoderFactory(),
        webrtc::CreateBuiltinVideoEncoderFactory(),
        webrtc::CreateBuiltinVideoDecoderFactory(), nullptr, nullptr);

    if (!pc_factory) {
      std::cerr << "Failed to create PeerConnectionFactory\n";
      std::exit(-1);
    }

    webrtc::PeerConnectionInterface::RTCConfiguration config{};
    config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
    webrtc::PeerConnectionInterface::IceServer turner{};
    turner.uri = "turn:54.200.166.206:3478?transport=tcp";
    turner.username = "user";
    turner.password = "root";
    config.servers.emplace_back(std::move(turner));

    const auto maybe_pc = pc_factory->CreatePeerConnectionOrError(
        config, webrtc::PeerConnectionDependencies{this});

    if (!maybe_pc.ok()) {
      std::cerr << "Failed to create PeerConnection\n";
      std::exit(-1);
    }

    peer_connection = std::move(maybe_pc.value());

    server.init_asio();
    server.set_message_handler(
        websocketpp::lib::bind(&observer::on_message, this, ::_1, ::_2));

    server.set_open_handler(
        websocketpp::lib::bind(&observer::on_open, this, ::_1));

    server.listen(9002);
    server.start_accept();
    server.run();
    stop();
  }

 private:
  server_type server{};
  rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection{};
  rtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track{};
  websocketpp::connection_hdl connection{};
  std::thread waiter_thread{};
  consumer_type& consumer{};

  void stop() {
    track = nullptr;
    peer_connection->Close();
    peer_connection = nullptr;
    waiter_thread.join();
  }

  void close() {
    if (!connection.expired()) {
      server.close(connection, websocketpp::close::status::going_away,
                   "Exited from console");

      connection = {};
    }
  }

  void on_message(websocketpp::connection_hdl hdl, message_ptr message) {
    if (hdl.lock() != connection.lock()) {
      std::cerr << "Wrong socket!\n";
      return;
    }

    if (message->get_opcode() != 1) {
      std::cerr << "I don't know how to use this frame\n";
      return;
    }

    auto payload = boost::json::parse(message->get_payload()).as_object();
    if (payload.contains("offer")) {
      auto offer = payload["offer"].as_object();

      peer_connection->SetRemoteDescription(
          this, webrtc::CreateSessionDescription(
                    webrtc::SdpTypeFromString(offer["type"].as_string().c_str())
                        .value(),
                    offer["sdp"].as_string().c_str())
                    .release());

      peer_connection->CreateAnswer(
          this, webrtc::PeerConnectionInterface::RTCOfferAnswerOptions{});
    } else if (payload.contains("new-ice-candidate")) {
      auto blob = payload["new-ice-candidate"].as_object();
      webrtc::SdpParseError error{};
      std::unique_ptr<webrtc::IceCandidateInterface> candidate{
          webrtc::CreateIceCandidate(blob["sdpMid"].as_string().c_str(),
                                     blob["sdpMLineIndex"].as_int64(),
                                     blob["candidate"].as_string().c_str(),
                                     &error)};

      if (!candidate) {
        std::cerr << "Failed to parse ICE candidate: " << error.description
                  << "\n";
        return;
      }

      peer_connection->AddIceCandidate(
          std::move(candidate), [](webrtc::RTCError error) {
            if (!error.ok()) {
              std::cerr << "[ERROR] Failed to set ICE candidate with error: "
                        << error.message() << "\n";
            }
          });
    }
  }

  void on_open(websocketpp::connection_hdl hdl) {
    if (connection.expired()) {
      std::cerr << "[INFO] Connection opened\n";
      connection = hdl;
    } else {
      std::cerr << "[WARNING] Rejecting connection\n";
      server.close(hdl, websocketpp::close::status::subprotocol_error,
                   "Rejected connection; other client already present");
    }
  }

  void OnSignalingChange(
      webrtc::PeerConnectionInterface::SignalingState new_state) override {}

  void OnDataChannel(
      rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override {}

  void OnIceGatheringChange(
      webrtc::PeerConnectionInterface::IceGatheringState state) override {
    std::cerr << "[WARNING] ICE gathering state change: " << [state] {
      switch (state) {
        case decltype(state)::kIceGatheringComplete:
          return "Complete";

        case decltype(state)::kIceGatheringGathering:
          return "Gathering";

        case decltype(state)::kIceGatheringNew:
          return "New";
      }
    }() << "\n";
  }

  void OnStandardizedIceConnectionChange(
      webrtc::PeerConnectionInterface::IceConnectionState state) override {
    std::cerr << "[INFO] ICE connection state change: " << [this, state] {
      switch (state) {
        case decltype(state)::kIceConnectionChecking:
          return "Checking";

        case decltype(state)::kIceConnectionClosed:
          close();
          return "Closed";

        case decltype(state)::kIceConnectionCompleted:
          return "Completed";

        case decltype(state)::kIceConnectionConnected:
          return "Connected";

        case decltype(state)::kIceConnectionDisconnected:
          close();
          return "Disconnected";

        case decltype(state)::kIceConnectionFailed:
          close();
          return "Failed";

        case decltype(state)::kIceConnectionMax:
          return "Max";

        case decltype(state)::kIceConnectionNew:
          return "New";
      }
    }() << "\n";
  }

  void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
    std::string blob{};
    if (!candidate->ToString(&blob)) {
      std::cerr << "[ERROR] Failed to serialize ICE candidate\n";
      return;
    }

    boost::json::object data{};
    boost::json::object inner_blob{};
    inner_blob["candidate"] = blob;
    inner_blob["sdpMid"] = candidate->sdp_mid();
    inner_blob["sdpMLineIndex"] = candidate->sdp_mline_index();
    data["iceCandidate"] = inner_blob;
    server.send(connection, boost::json::serialize(data),
                websocketpp::frame::opcode::text);
  }

  void OnConnectionChange(
      webrtc::PeerConnectionInterface::PeerConnectionState state) {
    std::cerr << "[INFO] Connection state change: " << [state] {
      switch (state) {
        case decltype(state)::kNew:
          return "New";

        case decltype(state)::kFailed:
          return "Failed";

        case decltype(state)::kDisconnected:
          return "Disconnected";

        case decltype(state)::kConnecting:
          return "Connecting";

        case decltype(state)::kConnected:
          return "Connected";

        case decltype(state)::kClosed:
          return "Closed";
      }
    }() << "\n";
  }

  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
    std::cerr << "[INFO] Created local session description\n";
    peer_connection->SetLocalDescription(this, desc);
    boost::json::object data{};
    data["type"] = webrtc::SdpTypeToString(desc->GetType());
    std::string sdp{};
    if (!desc->ToString(&sdp)) {
      std::cerr << "[ERROR] Failed to serialize SDP\n";
      return;
    }

    data["sdp"] = sdp;
    boost::json::object msg{};
    msg["answer"] = data;
    server.send(connection, boost::json::serialize(msg),
                websocketpp::frame::opcode::text);
  }

  void OnSuccess() override { std::cerr << "[INFO] Succeeded\n"; }

  void OnFailure(webrtc::RTCError error) override {
    std::cerr << "[ERROR] Failed: " << error.message() << "\n";
  }

  void OnTrack(rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver)
      override {
    std::cerr << "[INFO] Added track of type: "
              << cricket::MediaTypeToString(transceiver->media_type()) << "\n";
    track = transceiver->receiver()->track();
    if (track->enabled())
      std::cerr << "[INFO] Track is enabled\n";

    consumer.on_track(transceiver);
  }
};

struct null_consumer {
  void on_track(
      rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
    std::cerr << "[INFO] Null consumer saw new track\n";
  }
};

}  // namespace receiver

int main() {
  using namespace receiver;

  null_consumer consumer{};
  std::mutex exit_lock{};

  std::atomic_bool force_exit{};
  std::thread input_thread{[&exit_lock, &force_exit] {
    exit_lock.lock();
    std::string input{};
    while (input != "exit" && !std::cin.eof() && !force_exit)
      std::cin >> input;
    exit_lock.unlock();
  }};

  // Janky, but the idea is to wait for the lock to be held by input_thread;
  while (exit_lock.try_lock())
    exit_lock.unlock();

  const auto presenter_stream =
      rtc::make_ref_counted<observer<null_consumer>>(consumer, exit_lock);

  try {
    presenter_stream->start_signal_server();
  } catch (const websocketpp::exception& error) {
    std::cerr << error.what() << "\n";
    force_exit = true;
  }

  input_thread.join();

  return force_exit ? -1 : 0;
}
