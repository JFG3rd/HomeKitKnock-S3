sequenceDiagram
    autonumber

    participant ESP32 as ESP32 Doorbell
    participant FB as FRITZ!Box
    participant PH as Phones (**11)
    participant FF as FRITZ!Fon (Talk/Open Keys)

    %% --- Registration Phase ---
    ESP32->>FB: REGISTER (no auth)
    FB-->>ESP32: 401 Unauthorized\nWWW-Authenticate (realm, nonce, qop)

    ESP32->>ESP32: Compute Digest (REGISTER)
    ESP32->>FB: REGISTER (with digest auth)
    FB-->>ESP32: 200 OK (Registered)

    %% --- Idle ---
    ESP32->>ESP32: Wait for button press

    %% --- Call Setup ---
    ESP32->>FB: INVITE sip:**11@fritz.box (no auth)
    FB-->>ESP32: 401 Unauthorized\nWWW-Authenticate (realm, nonce, qop)

    ESP32->>ESP32: Compute Digest (INVITE)
    ESP32->>FB: INVITE (with digest auth)

    FB-->>ESP32: 100 Trying
    FB-->>PH: Trigger ringing for group **11
    FB-->>ESP32: 180 Ringing / 183 Session Progress

    %% --- FRITZ!Fon User Interaction ---
    FF-->>FB: TALK key pressed\n(Answer incoming call)
    FB-->>ESP32: 200 OK (Call answered)

    ESP32->>FB: ACK
    ESP32->>FB: Start RTP (audio stream)

    %% --- Active Call ---
    ESP32->>ESP32: In-call audio exchange

    %% --- Door Opening (Optional) ---
    FF-->>FB: OPEN key pressed\n(Sends DTMF or SIP INFO)
    FB-->>ESP32: DTMF/INFO event\n(Open door trigger)
    ESP32->>ESP32: Execute door-open action

    %% --- Hangup ---
    alt Remote hangs up (FRITZ!Fon)
        FF-->>FB: BYE
        FB-->>ESP32: BYE
        ESP32->>FB: 200 OK
    else Local hangs up (ESP32)
        ESP32->>FB: BYE
        FB-->>ESP32: 200 OK
    end

    ESP32->>ESP32: Stop RTP / Return to Registered
