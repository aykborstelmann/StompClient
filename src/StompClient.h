/**
   STOMPClient works with WebSocketsClient to provide a simple STOMP interface
   Note that there are several restrictions on the client's functionality

   With thanks to:

   Martin Becker : becker@informatik.uni-wuerzburg.de
   Kevin Hoyt    : parkerkrhoyt@gmail.com
*/

#ifndef STOMP_CLIENT
#define STOMP_CLIENT

#ifndef STOMP_MAX_SUBSCRIPTIONS
#define STOMP_MAX_SUBSCRIPTIONS 8
#endif

#include "Stomp.h"
#include "StompCommandParser.h"
#include <WebSocketsClient.h>

namespace Stomp {

    class StompClient {

    public:

        /**
           Constructs a new StompClient
           @param wsClient WebSocketsClient
           @param host char*                - The name of the host to connect to
           @param port int                  - The host port to use
           @param url char*                 - The url to contact to initiate the connection
           @param sockjs bool               - Set to true to indicate that the connection uses SockJS protocol
        */
        StompClient(
                WebSocketsClient &wsClient,
                const char *host,
                const int port,
                const char *url,
                const bool sockjs
        ) : _wsClient(wsClient), _host(host), _port(port), _url(url), _sockjs(sockjs), _user(nullptr), _id(0),
            _state(DISCONNECTED), _connectHandler(nullptr), _disconnectHandler(nullptr), _receiptHandler(nullptr),
            _errorHandler(nullptr), _heartbeats(0), _commandCount(0) {

            _wsClient.onEvent([this](WStype_t type, uint8_t *payload, size_t length) {
                this->_handleWebSocketEvent(type, payload, length);
            });

            for (auto &_subscription: _subscriptions) {
                _subscription.id = -1;
            }

        }

        ~StompClient() = default;

        /**
           Call this in the setup() routine to initiate the connection.
           This method initiates the websocket connection, waits for it to be set-up, then establishes the STOMP connection
        */
        void begin() {
            // connect to websocket
            _wsClient.begin(_host, _port, _socketUrl());
            _wsClient.setExtraHeaders();
        }

        void beginSSL() {
            // connect to websocket
            _wsClient.beginSSL(_host, _port, _socketUrl().c_str());
            _wsClient.setExtraHeaders();
        }

        void loop() {
            _wsClient.loop();
            _doHeartbeat();
        }

        /**
           Make a new subscription. Number incrementally
           Returns the id of the new subscription
           Returns -1 if maximum number of subscriptions (set by STOMP_MAX_SUBSCRIPTIONS) has been exceeded
           @param queue char*                 - The name of the queue to which to subscribe
           @param ackType Stomp_AckMode_t     - The acknowledgement mode to use for received messages
           @param handler StompMessageHandler - The callback function to execute when a message is received
           @return int                        - The numeric id of the subscription, or -1 if no slots are available
        */
        int subscribe(const String &queue, Stomp_AckMode_t ackType, StompMessageHandler handler) {
            // Scan for an unused subscription slot
            for (int i = 0; i < STOMP_MAX_SUBSCRIPTIONS; i++) {

                if (_subscriptions[i].id == -1) {

                    _subscriptions[i].id = i;
                    _subscriptions[i].messageHandler = handler;

                    String ack;
                    switch (ackType) {
                        case AUTO:
                            ack = "auto";
                            break;
                        case CLIENT:
                            ack = "client";
                            break;
                        case CLIENT_INDIVIDUAL:
                            ack = "client-individual";
                            break;
                    }

                    String lines[4] = {"SUBSCRIBE", "id:sub-" + String(i), "destination:" + queue, "ack:" + ack};
                    _send(lines, 4);

                    return i;
                }
            }
            return -1;
        }

        /**
           Cancel the given subscription
           @param subscription int - The subscription number previously returned by the subscribe() method
        */
        void unsubscribe(int subscription) {
            String msg[2] = {"UNSUBSCRIBE", "id:sub-" + String(subscription)};
            _send(msg, 2);

            _subscriptions[subscription].id = -1;
            _subscriptions[subscription].messageHandler = nullptr;
        }

        /**
         * Acknowledge receipt of the message
         * @param message StompCommand - The message being acknowledged
         */
        void ack(StompCommand message) {
            String msg[2] = {"ACK", "id:" + message.headers.getValue("ack")};
            _send(msg, 2);
        }

        /**
         * Reject receipt of the message with the given messageId
         * @param message StompCommand - The message being rejected
         */
        void nack(StompCommand message) {
            String msg[2] = {"NACK", "id:" + message.headers.getValue("ack")};
            _send(msg, 2);
        }

        void disconnect() {
            String msg[2] = {"DISCONNECT", "receipt:" + String(_commandCount)};
            _send(msg, 2);
        }

        void sendMessage(const String &destination, const String &message) {
            String lines[4] = {"SEND", "destination:" + destination, "", message};
            _send(lines, 4);
        }

        void sendMessageAndHeaders(const String &destination, const String &message, const StompHeaders &headers) {
            String lines[4] = {"SEND", "destination:" + destination, "", message};
            _sendWithHeaders(lines, 4, headers);
        }

        void onConnect(StompStateHandler handler) {
            _connectHandler = handler;
        }

        void onDisconnect(StompStateHandler handler) {
            _disconnectHandler = handler;
        }

        void onReceipt(StompStateHandler handler) {
            _receiptHandler = handler;
        }

        void onError(StompStateHandler handler) {
            _errorHandler = handler;
        }

        void setUser(const char *user) {
            _user = user;
        }

    private:
        const long _preferredHeartbeat = 10000;

        WebSocketsClient &_wsClient;
        const char *_host;
        const int _port;
        const char *_url;
        const bool _sockjs;
        const char *_user;

        long _id;
        unsigned long _lastSent = millis();
        unsigned long _heartbeatInterval = 0;

        Stomp_State_t _state;

        StompSubscription _subscriptions[STOMP_MAX_SUBSCRIPTIONS];

        StompStateHandler _connectHandler;
        StompStateHandler _disconnectHandler;
        StompStateHandler _receiptHandler;
        StompStateHandler _errorHandler;

        uint32_t _heartbeats;
        uint32_t _commandCount;

        String _socketUrl() {
            String socketUrl = _url;
            if (_sockjs) {
                socketUrl += random(0, 999);
                socketUrl += "/";
                socketUrl += random(0, 999999); // should be a random string, but this works (see )
                socketUrl += "/websocket";
            }

            return socketUrl;
        }

        void _handleWebSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
            String text = (char *) payload;
            Serial.println("Event");
            Serial.println(text);

            switch (type) {
                case WStype_DISCONNECTED:
                    _state = DISCONNECTED;
                    break;

                case WStype_CONNECTED:
                    _connectStomp();
                    break;

                case WStype_TEXT:

                    if (_sockjs) {
                        if (payload[0] == 'h') {
                            _heartbeats++;
                        } else if (payload[0] == 'o') {
                            _connectStomp();
                        } else if (payload[0] == 'a') {
                            StompCommand command = StompCommandParser::parse(text);
                            _handleCommand(command);
                        }
                    } else {
                        StompCommand command = StompCommandParser::parse(text);
                        _handleCommand(command);
                    }

                    break;

                default:
                    break;
            }
        }

        void _doHeartbeat() {
            if (_heartbeatInterval <= 0) {
                return;
            }

            if (millis() - _lastSent > _heartbeatInterval) {
                _sendHeartbeat();
            }
        }

        void _sendHeartbeat() {
            String msg = "\n";
            Serial.println("SENDING HEARTBEAT");
            Serial.println();

            _wsClient.sendTXT(msg.c_str(), msg.length());
            _lastSent = millis();
            _commandCount++;
        }

        void _connectStomp() {
            if (_state != OPENING) {
                _state = OPENING;

                String heartbeatHeader = "heart-beat:" + String(_preferredHeartbeat) + ",0";
                String msg[5] = {"CONNECT", "accept-version:1.1,1.0", heartbeatHeader};
                int current_length = 3;

                if (_user != nullptr) {
                    msg[current_length++] = "login:" + String(_user);
                }

                _send(msg, current_length);
            }
        }

        void _handleCommand(const StompCommand &command) {

            if (command.command.equals("CONNECTED")) {

                _handleConnected(command);

            } else if (command.command.equals("MESSAGE")) {

                _handleMessage(command);

            } else if (command.command.equals("RECEIPT")) {

                _handleReceipt(command);

            } else if (command.command.equals("ERROR")) {

                _handleError(command);

            } else {
                // discard unsupported command
            }
        }

        void _handleConnected(const StompCommand &command) {
            if (_state != CONNECTED) {
                _state = CONNECTED;
                parseHeartbeat(command);
                if (_connectHandler) {
                    _connectHandler(command);
                }
            }
        }

        void parseHeartbeat(const StompCommand &command) {
            StompHeaders headers = command.headers;
            const String &heartBeatHeader = headers.getValue("heart-beat");

            if (!String("").equals(heartBeatHeader)) {
                String heartbeatInterval = heartBeatHeader.substring(heartBeatHeader.indexOf(','),
                                                                     heartBeatHeader.length());
                _heartbeatInterval = max(heartBeatHeader.toInt(), _preferredHeartbeat);
                Serial.println("Got heartbeat interval: " + String(_heartbeatInterval));
                Serial.println();
            }
        }

        void _handleMessage(StompCommand message) {
            String sub = message.headers.getValue("subscription");
            if (!sub.startsWith("sub-")) {
                // Not for us. Do nothing (raise an error one day??)
                return;
            }
            long id = sub.substring(4).toInt();

            String messageId = message.headers.getValue("message-id");

            StompSubscription *subscription = &_subscriptions[id];
            if (subscription->id != id) {
                return;
            }

            if (subscription->messageHandler) {
                StompMessageHandler callback = subscription->messageHandler;
                Stomp_Ack_t ackType = callback(message);
                switch (ackType) {
                    case ACK:
                        ack(message);
                        break;

                    case NACK:
                        nack(message);
                        break;

                    case CONTINUE:
                    default:
                        break;
                }
            }

        }

        void _handleReceipt(const StompCommand &command) {

            if (_receiptHandler) {
                _receiptHandler(command);
            }

            if (_state == DISCONNECTING) {
                _state = DISCONNECTED;
                if (_disconnectHandler) {
                    _disconnectHandler(command);
                }
            }
        }

        void _handleError(const StompCommand &command) {
            _state = DISCONNECTED;
            if (_errorHandler) {
                _errorHandler(command);
            }
            if (_disconnectHandler) {
                _disconnectHandler(command);
            }
        }

        void _send(String lines[], uint8_t nlines) {
            String msg;
            for (int i = 0; i < nlines; i++) {
                msg += lines[i];
                msg += "\n";
            }
            msg += "\n";

            Serial.println("SENDING MESSAGE:");
            Serial.println(msg);

            _wsClient.sendTXT(msg.c_str(), msg.length() + 1);
            _lastSent = millis();
            _commandCount++;
        }

        void _sendWithHeaders(String lines[], uint8_t nlines, StompHeaders headers) {

            String msg;
            // Add the command
            msg += lines[0] + "\n";
            // Add the extra headers
            for (int i = 0; i < headers.size(); i++) {
                StompHeader h = headers.get(i);
                msg += h.key + ":" + h.value + "\n";
            }
            // Now the rest of the message
            for (int i = 1; i < nlines; i++) {
                msg += lines[i];
                msg += "\n";
            }
            msg += "\n";

            _wsClient.sendTXT(msg.c_str(), msg.length() + 1);
            _commandCount++;
        }

    };

}

#endif
