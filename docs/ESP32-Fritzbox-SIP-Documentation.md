ESP32-FritzBox-SIP-Doorbell.md

⚠️ **STATUS:** SIP code complete and compiles, but cannot test due to NVS boot loop preventing WiFi initialization. See [IMPLEMENTATION_SUMMARY.md](IMPLEMENTATION_SUMMARY.md) for details on current blocker.

---

## 1. Correct ESP32 SIP INVITE with full digest auth
1.1 Initial unauthenticated INVITE

INVITE sip:**11@fritz.box SIP/2.0
Via: SIP/2.0/UDP 192.168.178.188:5062;branch=z9hG4bK-11111111
Max-Forwards: 70
From: "ESP32-Doorbell" <sip:SIPuser620@fritz.box>;tag=abc12345
To: <sip:**11@fritz.box>
Call-ID: 5ad09633@192.168.178.188
CSeq: 1 INVITE
Contact: <sip:SIPuser620@192.168.178.188:5062>
User-Agent: ESP32-Doorbell/1.0
Content-Type: application/sdp
Content-Length: 0

1.2 401 Unauthorized from FRITZ!Box

SIP/2.0 401 Unauthorized
WWW-Authenticate: Digest realm="fritz.box", nonce="5FCC4D494638D516", qop="auth"

1.3 Digest calculation formulas

HA1 = MD5(username:realm:password)
HA2 = MD5(method:uri)
response = MD5(HA1:nonce:nc:cnonce:qop:HA2)

1.4 Authenticated INVITE

INVITE sip:**11@fritz.box SIP/2.0
Via: SIP/2.0/UDP 192.168.178.188:5062;branch=z9hG4bK-22222222
Max-Forwards: 70
From: "ESP32-Doorbell" <sip:SIPuser620@fritz.box>;tag=abc12345
To: <sip:**11@fritz.box>
Call-ID: 5ad09633@192.168.178.188
CSeq: 2 INVITE
Contact: <sip:SIPuser620@192.168.178.188:5062>
Authorization: Digest username="SIPuser620", realm="fritz.box", nonce="5FCC4D494638D516", uri="sip:fritz.box", cnonce="a1b2c3d4e5f6g7h8", nc=00000001, qop=auth, response="edf91a7cd037cfcda343eb3d74ad4943", algorithm=MD5
User-Agent: ESP32-Doorbell/1.0
Content-Type: application/sdp
Content-Length: 0


2. Flowchart for correct SIP sequence with FRITZ!Box
Registration Flow
• STEP 1: START
• STEP 2: Load SIP config
• STEP 3: Send REGISTER (no auth)
• STEP 4: Receive 401 Unauthorized
• STEP 5: Compute digest
• STEP 6: Send REGISTER with Authorization
• STEP 7: Receive 200 OK
Call Flow
• STEP 8: WAIT FOR BUTTON PRESS
• STEP 9: Build INVITE to sip:**11@fritz.box
• STEP 10: Send INVITE (no auth)
• STEP 11: Receive 401 Unauthorized
• STEP 12: Compute digest for INVITE
• STEP 13: Send INVITE with Authorization
• STEP 14: Receive 100 Trying
• STEP 15: Receive 180 Ringing / 183 Session Progress
• STEP 16: Receive 200 OK
• STEP 17: Send ACK
• STEP 18: Start RTP audio
Hangup Flow
• STEP 19: WAIT FOR HANGUP
• STEP 20: If ESP32 ends call → send BYE
• STEP 21: If remote ends call → receive BYE
• STEP 22: Return to idle

3. Debug checklist to confirm routing to **11
• ESP32 appears under Telefoniegeräte → Türsprechanlage
• Internal number (e.g., **620) is assigned
• Target number is **11
• Phones are assigned to **11
• SIP registration is active
• Call list shows **620 → **11
• INVITE with auth is accepted
• If only email arrives → INVITE auth or routing failed
---
4. ESP32 SIP pseudo-code

setup() {
  loadConfig();
  sip.register();
}

loop() {
  if (buttonPressed()) {
    callDoorGroup();
  }
  sip.handleIncoming();
}

function sip.register() {
  sendRegister(noAuth);
  if (recv401()) {
    auth = computeDigest("REGISTER");
    sendRegister(auth);
  }
  waitFor200OK();
}

function callDoorGroup() {
  sendInvite(noAuth, target="**11");
  if (recv401()) {
    auth = computeDigest("INVITE");
    sendInvite(auth, target="**11");
  }
  waitForTrying();
  waitForRingingOrProgress();
  if (recv200OK()) sendACK();
  startRTP();
}

function handleHangup() {
  if (remoteSendsBYE()) send200OK();
  if (localHangup()) sendBYE();
}
