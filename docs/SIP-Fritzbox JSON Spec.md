{
  "module": "ESP32_FritzBox_SIP_Doorbell",
  "version": "1.0",
  "description": "SIP doorbell client for FRITZ!Box with digest authentication and call routing to **11.",
  "network": {
    "local_ip": "192.168.178.188",
    "local_port": 5062,
    "registrar": "fritz.box",
    "transport": "UDP"
  },
  "sip_user": {
    "username": "SIPuser620",
    "password": "<SECRET>",
    "internal_number": "**620",
    "target_number": "**11"
  },
  "sip_states": [
    "IDLE",
    "REGISTERING",
    "REGISTERED",
    "INVITING",
    "RINGING",
    "IN_CALL",
    "TERMINATING"
  ],
  "sip_messages": {
    "register_initial": {
      "method": "REGISTER",
      "uri": "sip:fritz.box",
      "auth": "none"
    },
    "register_authenticated": {
      "method": "REGISTER",
      "uri": "sip:fritz.box",
      "auth": "digest"
    },
    "invite_initial": {
      "method": "INVITE",
      "request_uri": "sip:**11@fritz.box",
      "auth": "none"
    },
    "invite_authenticated": {
      "method": "INVITE",
      "request_uri": "sip:**11@fritz.box",
      "auth": "digest"
    },
    "ack": {
      "method": "ACK"
    },
    "bye": {
      "method": "BYE"
    }
  },
  "digest_auth": {
    "algorithm": "MD5",
    "qop": "auth",
    "fields_required": [
      "username",
      "realm",
      "nonce",
      "uri",
      "cnonce",
      "nc",
      "qop",
      "response"
    ],
    "hash_formulas": {
      "HA1": "MD5(username:realm:password)",
      "HA2": "MD5(method:uri)",
      "response": "MD5(HA1:nonce:nc:cnonce:qop:HA2)"
    }
  },
  "flow": {
    "registration": [
      "SEND REGISTER (no auth)",
      "RECV 401 Unauthorized",
      "PARSE realm, nonce, qop",
      "COMPUTE digest for REGISTER",
      "SEND REGISTER (with auth)",
      "RECV 200 OK",
      "STATE = REGISTERED"
    ],
    "call_sequence": [
      "WAIT for button press",
      "SEND INVITE (no auth)",
      "RECV 401 Unauthorized",
      "PARSE realm, nonce, qop",
      "COMPUTE digest for INVITE",
      "SEND INVITE (with auth)",
      "RECV 100 Trying",
      "RECV 180 Ringing or 183 Session Progress",
      "RECV 200 OK",
      "SEND ACK",
      "START RTP",
      "STATE = IN_CALL"
    ],
    "hangup": [
      "IF remote sends BYE → SEND 200 OK",
      "IF local hangup → SEND BYE → RECV 200 OK",
      "STOP RTP",
      "STATE = REGISTERED"
    ]
  },
  "debug_checklist": {
    "fritzbox_config": [
      "Doorbell appears under Telefoniegeräte → Türsprechanlage",
      "Internal number (e.g., **620) assigned",
      "Target number set to **11",
      "Phones assigned to **11"
    ],
    "sip_status": [
      "REGISTER returns 200 OK",
      "INVITE with digest returns 100/180/183/200",
      "Call list shows **620 → **11"
    ],
    "failure_modes": {
      "email_only": "INVITE not authenticated or routing failed",
      "repeated_401": "Digest calculation incorrect",
      "no_ringing": "Target number **11 not assigned to phones"
    }
  }
}
