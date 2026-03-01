{
  "sip_stack": {
    "version": "1.0",
    "role": "UAC_with_minimal_UAS_for_BYE",
    "transport": "udp",
    "local": {
      "ip": "dynamic_dhcp",
      "sip_port": 5062,
      "rtp_port": 40000
    },

    "registration": {
      "registrar_uri": "sip:fritz.box",
      "user": "SIPuser620",
      "auth_user": "SIPuser620",
      "password": "<secret>",
      "expires": 300,
      "retry_interval_ms": 10000,
      "keepalive_interval_ms": 25000,
      "state_machine": [
        "UNREGISTERED",
        "REGISTERING",
        "WAIT_401",
        "REGISTERING_AUTH",
        "REGISTERED"
      ]
    },

    "call": {
      "target_uri": "sip:**11@fritz.box",
      "codec": "PCMU",
      "ptime_ms": 20,
      "dtmf_mode": "rtp",
      "dtmf_open_sequence": "123",
      "state_machine": [
        "IDLE",
        "INVITE_SENT",
        "WAIT_401",
        "INVITE_SENT_AUTH",
        "RINGING",
        "IN_CALL",
        "TERMINATING"
      ]
    },

    "transactions": {
      "branch_generation": "random_hex_8bytes",
      "call_id_generation": "random_hex_8bytes@local_ip",
      "cseq": {
        "register": "increment_per_request",
        "invite": "increment_per_request",
        "bye": "increment_per_request"
      },
      "timers": {
        "t1_ms": 500,
        "t2_ms": 4000,
        "invite_timeout_ms": 30000,
        "no_rtp_timeout_ms": 60000
      }
    },

    "digest_auth": {
      "algorithm": "MD5",
      "qop": "auth",
      "fields_required": [
        "realm",
        "nonce",
        "uri",
        "username",
        "password",
        "method",
        "nc",
        "cnonce"
      ],
      "nc_format": "00000001",
      "cnonce_generation": "random_hex_8bytes",
      "response_formula": "MD5( MD5(username:realm:password) : nonce : nc : cnonce : qop : MD5(method:uri) )"
    },

    "messages": {
      "REGISTER": {
        "method": "REGISTER",
        "uri": "sip:fritz.box",
        "headers": [
          "Via",
          "Max-Forwards",
          "From",
          "To",
          "Call-ID",
          "CSeq",
          "Contact",
          "Expires",
          "User-Agent",
          "Authorization(optional)"
        ]
      },
      "INVITE": {
        "method": "INVITE",
        "uri": "sip:**11@fritz.box",
        "headers": [
          "Via",
          "Max-Forwards",
          "From",
          "To",
          "Call-ID",
          "CSeq",
          "Contact",
          "Content-Type: application/sdp",
          "Authorization(optional)"
        ],
        "body": "sdp_offer_pcmu"
      },
      "ACK": {
        "method": "ACK",
        "uri": "remote_contact_uri",
        "headers": [
          "Via",
          "From",
          "To",
          "Call-ID",
          "CSeq",
          "Max-Forwards"
        ]
      },
      "BYE": {
        "method": "BYE",
        "uri": "remote_contact_uri",
        "headers": [
          "Via",
          "From",
          "To",
          "Call-ID",
          "CSeq",
          "Max-Forwards",
          "Authorization(optional)"
        ]
      }
    },

    "rtp": {
      "codec": "PCMU",
      "sample_rate_hz": 8000,
      "ptime_ms": 20,
      "direction": "sendrecv",
      "callbacks": {
        "on_start": "rtp_start(remote_ip, remote_port)",
        "on_stop": "rtp_stop()",
        "on_tick": "rtp_send_pcmu_frame()",
        "on_dtmf": "rtp_send_dtmf_event(digit)"
      }
    },

    "events": {
      "on_registered": "callback()",
      "on_registration_failed": "callback(code)",
      "on_call_ringing": "callback()",
      "on_call_established": "callback()",
      "on_call_ended": "callback()",
      "on_dtmf_received": "callback(digit)",
      "on_error": "callback(code)"
    },

    "api": {
      "init": "sip_init(config)",
      "loop": "sip_poll()",
      "start_call": "sip_invite()",
      "hangup": "sip_bye()",
      "send_dtmf": "sip_send_dtmf(digit)"
    }
  }
}
