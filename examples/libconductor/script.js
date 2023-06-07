let socket = null;

function openSignallingChannel() {
    //let ip = "18.236.77.108";
    let ip = "localhost";
    let new_socket = new WebSocket(`ws://${ip}:${document.getElementById("port").value}`);
    new_socket.onopen = () => {
        socket = new_socket;
        disable();
        if (!socket) {
            alert("No signalling socket");
            return;
        }

        makeCall();
    };

    new_socket.onerror = (_, event) => {
        alert("Couldn't connect");
    }
}

function disable() {
    let button = document.getElementById("callButton");
    button.textContent = "Connected!";
    button.disabled = true;
}

function enable() {
    let button = document.getElementById("callButton");
    button.textContent = "Share Screen";
    button.disabled = false;
}

function callReceiver() {
    openSignallingChannel();
}

async function makeCall() {
    const configuration = {
        iceServers: [
            {
                urls: "turn:54.200.166.206:3478?transport=tcp",
                username: "user",
                credential: "root"
            }
        ]
    };

    const send = obj => socket.send(JSON.stringify(obj));
    const peerConnection = new RTCPeerConnection(configuration);

    peerConnection.ontrack = event => alert("TRACK");
    peerConnection.ondatachannel = event => alert("CHANNEL");
    peerConnection.onconnectionstatechange = event => {
        console.log(peerConnection.connectionState);
        if (peerConnection.connectionState == "disconnected") {
            socket.close();
            socket = null;
            enable();
            peerConnection.close();
            if (stream != null) {
                stream.getTracks().forEach(track => { track.stop(); });
            }
        }
    };

    let makingOffer = false;
    peerConnection.onnegotiationneeded = async () => {
        try {
            makingOffer = true;
            await peerConnection.setLocalDescription();
            send({ description: peerConnection.localDescription });
        } catch (err) {
            console.error(err);
        } finally {
            makingOffer = false;
        }
    };

    let ignoreOffer = false;
    const polite = true;
    socket.onmessage = async message => {
        message = JSON.parse(message.data);
        let inner = async ({ description, candidate }) => {
            try {
                if (description) {
                    const offerCollision =
                        description.type === "offer" &&
                        (makingOffer || peerConnection.signalingState !== "stable");

                    ignoreOffer = !polite && offerCollision;
                    if (ignoreOffer) {
                        return;
                    }

                    await peerConnection.setRemoteDescription(description);
                    if (description.type === "offer") {
                        await peerConnection.setLocalDescription();
                        send({ description: peerConnection.localDescription });
                    }
                } else if (candidate) {
                    console.log(candidate);
                    try {
                        await peerConnection.addIceCandidate(candidate);
                    } catch (err) {
                        if (!ignoreOffer) {
                            throw err;
                        }
                    }
                }
            } catch (err) {
                console.error(err);
            }
        };

        await inner(message);
    };

    peerConnection.onicecandidate = ({ candidate }) => { if (candidate) send({ candidate }); };

    peerConnection.onicegatheringstatechange = _ => {
        console.log("[ICE] " + peerConnection.iceGatheringState);
    }

    peerConnection.oniceconnectionstatechange = _ => {
        console.log("[ICE Connection] " + peerConnection.iceConnectionState);
    }

    let stream = null;
    if (document.getElementById("port").value == "9002") {
        console.log("Adding stream");
        stream = await navigator.mediaDevices.getDisplayMedia({
            video: {
                cursor: "always"
            },
            audio: false
        });

        stream.getTracks().forEach(track => { peerConnection.addTrack(track); });
        console.log("Added track");
    } else {
        console.log("Adding data channel");
        peerConnection.addTransceiver("video", { direction: "recvonly" })
        peerConnection.createDataChannel("dummyChannel");
    }
}
