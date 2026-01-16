classDiagram
    class SIPClient {
        +string username
        +string password
        +string registrar
        +string localIP
        +int localPort
        +register()
        +invite(target)
        +computeDigest(method, uri, nonce)
        +sendRequest(method, headers, body)
        +handleResponse()
    }

    class DigestAuth {
        +string realm
        +string nonce
        +string qop
        +string cnonce
        +string nc
        +computeHA1(username, realm, password)
        +computeHA2(method, uri)
        +computeResponse(HA1, nonce, nc, cnonce, qop, HA2)
    }

    class SIPStateMachine {
        +enum State {IDLE, REGISTERING, REGISTERED, INVITING, RINGING, IN_CALL, TERMINATING}
        +State currentState
        +transition(event)
    }

    class RTPHandler {
        +start()
        +stop()
        +sendAudio()
        +receiveAudio()
    }

    SIPClient --> DigestAuth : uses
    SIPClient --> SIPStateMachine : controls
    SIPClient --> RTPHandler : manages
