sequenceDiagram
    autonumber

    participant ESP32 as ESP32 Doorbell
    participant FB as FRITZ!Box
    participant PH as Phones (**11)

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

    PH-->>FB: 200 OK (Call answered)
    FB-->>ESP32: 200 OK (Call established)

    ESP32->>FB: ACK
    ESP32->>FB: Start RTP (audio stream)

    %% --- Active Call ---
    ESP32->>ESP32: In-call audio exchange

    %% --- Hangup ---
    alt Remote hangs up
        PH-->>FB: BYE
        FB-->>ESP32: BYE
        ESP32->>FB: 200 OK
    else Local hangs up
        ESP32->>FB: BYE
        FB-->>ESP32: 200 OK
    end

    ESP32->>ESP32: Stop RTP / Return to Registered
