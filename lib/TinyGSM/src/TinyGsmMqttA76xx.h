/**
 * @file      TinyGsmMqttA76xx.h
 * @author    Lewis He (lewishe@outlook.com)
 * @license   LGPL-3.0
 * @date      2023-11-29
 *
 */
#pragma once

#include "TinyGsmCommon.h"

#define TINY_GSM_MQTT_CLI_COUNT 2

template <class modemType, uint8_t muxCount>
class TinyGsmMqttA76xx
{
public:
    typedef void (*callback_t)(const char *, const uint8_t *, uint32_t);
protected:
    bool __ssl = false;
    bool __sni = false;
    uint8_t *buffer = NULL;
    uint32_t bufferSize = 256;
    callback_t callback;
    const char  *cert_pem;           /*!< SSL server certification, PEM format as string, if the client requires to verify server */
    const char  *client_cert_pem;    /*!< SSL client certification, PEM format as string, if the server requires to verify client */
    const char  *client_key_pem;     /*!< SSL client key, PEM format as string, if the server requires to verify client */
    const char  *will_topic;
    const char  *will_msg;
    uint8_t will_qos = 0;
    bool _isConnected = false;
    uint32_t _lastCheckConnect = 0;

    // --- publish 완료(+CMQTTPUB URC) 추적 -----------------------------------
    // mqtt_publish() 는 AT+CMQTTPUB 이 OK 를 내면 곧바로 true 를 돌려준다. 실제 발행
    // 결과는 그 뒤 비동기로 오는 URC `+CMQTTPUB: <client_index>,<err>` 에만 들어 있고,
    // 원본 래퍼는 그걸 파싱하지 않아 "### Unhandled" 로 버렸다 — 즉 반환값 true 는
    // "AT 를 받아들였다"이지 "브로커에 전달됐다"가 아니다.
    // 아래 값들을 waitResponse 의 URC 분기가 채우고, 응용이 조회해 실제 완료를 판정한다.
    // 인증서를 이번 부팅에 이미 모뎀에 올렸는가. CCERTDOWN 은 모뎀 플래시에 쓰므로
    // 재접속마다 반복할 이유가 없다 — PEM 전송(UART) + 플래시 쓰기를 매번 되풀이하면
    // 재접속이 느려지고 모뎀 플래시 수명만 깎인다.
    bool     _certUploaded  = false;

    bool     _pubAckPending = false;   // 발행을 내보내고 결과 URC 를 기다리는 중
    int8_t   _lastPubErr    = -1;      // 마지막 결과 코드 (0=성공, -1=아직 없음)
    uint32_t _pubSentAt     = 0;       // 발행 시각(millis) — 응용이 타임아웃 판정에 쓴다

public:
    // 결과 URC 수신 — waitResponse 의 `+CMQTTPUB:` 분기가 호출한다.
    void mqttNotePubAck(int8_t err)
    {
        _lastPubErr    = err;
        _pubAckPending = false;
    }
    // 발행을 내보낸 뒤 아직 결과 URC 가 오지 않았는가.
    bool     mqttPubAckPending() const { return _pubAckPending; }
    // 마지막 발행 결과 코드. 0 = 성공, 그 외 = 모뎀 에러, -1 = 아직 결과 없음.
    int8_t   mqttLastPubErr()    const { return _lastPubErr; }
    // 마지막 발행을 내보낸 시각(millis).
    uint32_t mqttPubSentAt()     const { return _pubSentAt; }
    /*
     * Basic functions
     */
    bool mqtt_begin(bool ssl, bool sni = false)
    {
        // Clean up previous MQTT connection if any
        thisModem().sendAT("+CMQTTDISC=0,120");
        thisModem().waitResponse();
        thisModem().sendAT("+CMQTTDISC=1,120");
        thisModem().waitResponse();
        thisModem().sendAT("+CMQTTREL=0,120");
        thisModem().waitResponse();
        thisModem().sendAT("+CMQTTREL=1,120");
        thisModem().waitResponse();
        thisModem().sendAT("+CMQTTSTOP");
        thisModem().waitResponse();
        delay(20);  //Wait 20 ms

        __sni = sni;
        __ssl = ssl;
        if (!this->buffer) {
            this->buffer = (uint8_t *)TINY_GSM_MALLOC(bufferSize);
            if (!this->buffer)return false;
        }
        this->cert_pem = NULL;
        this->client_cert_pem = NULL;
        this->client_key_pem = NULL;
        _isConnected = false;
        _lastCheckConnect = 0;
        memset(this->buffer, 0, bufferSize);
        thisModem().sendAT("+CMQTTSTART");
        if (thisModem().waitResponse(30000UL, "+CMQTTSTART: 0") != 1)return false;
        thisModem().waitResponse();
        return true;
    }

    bool mqtt_end()
    {
        if (this->buffer) {
            free(this->buffer);
            this->buffer = NULL;
        }
        _isConnected = false;
        _lastCheckConnect = 0;
        thisModem().sendAT("+CMQTTSTOP");
        thisModem().waitResponse("+CMQTTSTOP: 0");
        if (thisModem().waitResponse(3000) != 1)return false;
        return true;
    }

    // caFile:           /*!< SSL server certification, PEM format as string, if the client requires to verify server */
    // clientCertFile:    /*!< SSL client certification, PEM format as string, if the server requires to verify client */
    // clientCertKey:     /*!< SSL client key, PEM format as string, if the server requires to verify client */
    void mqtt_set_certificate(const char *caFile,
                              const char *clientCertFile = NULL,
                              const char *clientCertKey = NULL)
    {
        // 인증서 내용이 바뀌면 모뎀에 다시 올려야 한다 — 포인터가 달라질 때만 무효화한다.
        // 같은 포인터로 매 접속 재지정하는 흔한 사용법에서는 재업로드가 일어나지 않는다.
        if (caFile != this->cert_pem || clientCertFile != this->client_cert_pem ||
            clientCertKey != this->client_key_pem) {
            _certUploaded = false;
        }
        this->cert_pem = caFile;
        this->client_cert_pem = clientCertFile;
        this->client_key_pem = clientCertKey;
    }

    // 다음 mqtt_connect 에서 인증서를 강제로 다시 올린다(모뎀 파일 손실/공장초기화 대비).
    void mqttInvalidateCert() { _certUploaded = false; }

    void setWillMessage(const char *topic, const char *msg, uint8_t qos)
    {
        will_msg = msg;
        will_topic = topic;
        will_qos = qos;
    }

    bool mqtt_connect(
        uint8_t clientIndex,
        const char *server, uint16_t port,
        const char *clientID,
        const char *username = NULL,
        const char *password = NULL,
        uint32_t keepalive_time = 60)
    {
        uint8_t authMethod = 0;

        if (clientIndex > muxCount) {
            return false;
        }

        if (this->cert_pem || this->client_cert_pem || this->client_key_pem) {
            if (this->cert_pem) {
                // 업로드는 부팅당 1회. 모뎀 플래시에 남으므로 모뎀 리셋도 견딘다.
                // 실패하면 플래그를 세우지 않아 다음 시도가 자동으로 다시 올린다.
                if (!_certUploaded) {
                    thisModem().sendAT("+CCERTDOWN=\"ca_cert.pem\",", strlen(this->cert_pem));
                    if (thisModem().waitResponse(10000UL, ">") == 1) {
                        thisModem().stream.write(this->cert_pem);
                    }
                    if (thisModem().waitResponse() != 1) {
                        ESP_LOGE("A76XX", "Write ca_cert pem failed!");
                        return false;
                    }
                    _certUploaded = true;
                }
                // ⚠️ CSSLCFG 는 매번 보낸다. 파일은 플래시에 남지만 SSL 컨텍스트 설정은
                //    휘발성이라, 모뎀이 리셋되면 "어느 파일을 CA 로 쓸지"가 사라진다.
                thisModem().sendAT("+CSSLCFG=\"cacert\",0,\"ca_cert.pem\"");
                thisModem().waitResponse();

            }
            if (this->client_cert_pem) {
                thisModem().sendAT("+CCERTDOWN=\"cert.pem\",", strlen(this->client_cert_pem));
                if (thisModem().waitResponse(10000UL, ">") == 1) {
                    thisModem().stream.write(this->client_cert_pem);
                }
                if (thisModem().waitResponse() != 1) {
                    ESP_LOGE("A76XX", "Write cert pem failed!");
                    return false;
                }
                thisModem().sendAT("+CSSLCFG=\"clientcert\",0,\"cert.pem\"");
                thisModem().waitResponse();

            }
            if (this->client_key_pem) {
                thisModem().sendAT("+CCERTDOWN=\"key_cert.pem\",", strlen(this->client_key_pem));
                if (thisModem().waitResponse(10000UL, ">") == 1) {
                    thisModem().stream.write(this->client_key_pem);
                }
                if (thisModem().waitResponse() != 1) {
                    ESP_LOGE("A76XX", "Write key_cert failed!");
                    return false;
                }
                thisModem().sendAT("+CSSLCFG=\"clientkey\",0,\"key_cert.pem\"");
                thisModem().waitResponse();
            }

            // authMethod:
            // 0 – no authentication.
            // 1 – server authentication. It needs the root CA of the server.
            // 2 – server and client authentication. It needs the root CA of the server, the cert and key of the client.
            // 3 – client authentication and no server authentication. It needs the cert and key of the client.
            if (this->client_cert_pem && this->client_key_pem && !this->cert_pem) {
                authMethod = 3;
            } else if (this->client_cert_pem && this->client_key_pem && this->cert_pem) {
                authMethod = 2;
            }  else if (this->cert_pem) {
                authMethod = 1;
            } else {
                authMethod = 0;
            }

            thisModem().sendAT("+CSSLCFG=\"sslversion\",0,4");
            thisModem().waitResponse();

            thisModem().sendAT("+CMQTTSSLCFG=", clientIndex, ",", 0);
            thisModem().waitResponse(3000);

            __ssl = true;
        }

        // Some MQTT brokers need to enable sni
        if (__sni) {
            thisModem().sendAT("+CSSLCFG=\"enableSNI\",0,1");
            thisModem().waitResponse();
        }

        thisModem().sendAT("+CSSLCFG=\"authmode\",0,", authMethod);
        thisModem().waitResponse();

        thisModem().sendAT("+CMQTTREL=", clientIndex);
        thisModem().waitResponse(3000);

        thisModem().sendAT("+CMQTTACCQ=", clientIndex, ",\"", clientID, "\",", __ssl);
        if (thisModem().waitResponse(3000) != 1)return false;

        // Set MQTT3.1.1 , Default use MQTT 3.1
        thisModem().sendAT("+CMQTTCFG=\"version\",", clientIndex, ",4");
        thisModem().waitResponse(30000UL);

        if (will_msg && will_topic) {
            if (!mqttWillTopic(clientIndex, will_topic)) {
                return false;
            }
            if (!mqttWillMessage(clientIndex, will_msg, will_qos)) {
                return false;
            }
        }

        if (username && password) {
            thisModem().sendAT("+CMQTTCONNECT=", clientIndex, ',', "\"tcp://", server, ':', port, "\",", keepalive_time, ',', 1, ",\"", username, "\",\"", password, "\"");
        } else {
            thisModem().sendAT("+CMQTTCONNECT=", clientIndex, ',', "\"tcp://", server, ':', port, "\",", keepalive_time, ',', 1);
        }
        // ⚠️ 접속이 실패하면 인증서 캐시를 무효화한다. 모뎀 파일이 사라졌거나(공장초기화,
        //    플래시 손상) CA 가 잘못 올라간 경우, 캐시를 믿고 재업로드를 건너뛰면 영영
        //    TLS 가 안 붙는다 — 다음 시도가 다시 올리게 해 스스로 회복시킨다.
        if (thisModem().waitResponse(30000UL) != 1) { _certUploaded = false; return false; }

        if (thisModem().waitResponse(30000UL, "+CMQTTCONNECT: ") != 1) {
            _certUploaded = false;
            return false;
        }
        thisModem().streamSkipUntil(',');
        int res = thisModem().stream.read();
        if (res != '0') { _certUploaded = false; return false; }
        return true;

    }

    int mqtt_disconnect(uint8_t clientIndex = 0, uint32_t timeout = 120)
    {
        if (clientIndex > muxCount) {
            return false;
        }
        thisModem().sendAT("+CMQTTDISC=", clientIndex, ',', timeout);
        thisModem().waitResponse(3000);
        thisModem().waitResponse(10000UL, "+CMQTTDISC: ");
        // int id = thisModem().streamGetIntBefore(',');
        thisModem().streamSkipUntil(',');
        int status = thisModem().streamGetIntBefore('\n');
        thisModem().stream.flush();

        thisModem().sendAT("+CMQTTREL=", clientIndex);
        thisModem().waitResponse(3000);
        thisModem().sendAT("+CMQTTSTOP");
        thisModem().waitResponse("+CMQTTSTOP: ");
        status = thisModem().streamGetIntBefore('\n');
        if (thisModem().waitResponse(3000) != 1)return false;
        return status;
    }

    bool mqtt_publish(uint8_t clientIndex, const char *topic, const char *playload,
                      uint8_t qos = 0, uint32_t timeout = 60, uint8_t retain = 0)
    {
        if (clientIndex > muxCount) {
            return false;
        }
        // +CMQTTTOPIC: (0-1),(1-1024)
        // <client_index>,<req_length>
        //  publish message topic
        thisModem().sendAT("+CMQTTTOPIC=", clientIndex, ',', strlen(topic));
        if (thisModem().waitResponse(10000UL, ">") != 1) {
            return false;
        }
        thisModem().stream.write(topic);
        thisModem().stream.println();

        if (thisModem().waitResponse() != 1) {
            return false;
        }

        // AT+CMQTTPAYLOAD Input the publish message body
        // +CMQTTPAYLOAD: (0-1),(1-10240)
        // <client_index>,<req_length>
        thisModem().sendAT("+CMQTTPAYLOAD=", clientIndex, ',', strlen(playload));
        if (thisModem().waitResponse(10000UL, ">") != 1) {
            return false;
        }
        thisModem().stream.write(playload);
        thisModem().stream.println();
        // Wait return OK
        if (thisModem().waitResponse() != 1) {
            return false;
        }
        // +CMQTTPUB: (0-1),(0-2),(60-180),(0-1),(0-1)
        // <client_index>,<qos>,<pub_timeout>,<ratained>,<dup>
        thisModem().sendAT("+CMQTTPUB=", clientIndex, ',', qos, ',', timeout, ',', retain );
        if (thisModem().waitResponse() != 1) {
            return false;
        }
        // ⚠️ 여기서의 true 는 "모뎀이 발행 요청을 받아들였다"까지다. 실제 전달 결과는
        //    비동기 URC `+CMQTTPUB: <client_index>,<err>` 로 온다 — 아래 플래그를 세워
        //    응용이 그 URC 도착 여부로 진짜 완료를 판정하게 한다.
        _pubAckPending = true;
        _lastPubErr    = -1;
        _pubSentAt     = millis();
        return true;
    }

    bool mqtt_subscribe(uint8_t clientIndex, const char *topic, uint8_t qos = 0, uint8_t dup = 0)
    {
        if (clientIndex > muxCount) {
            return false;
        }
        // Subscribe a message to server
        // +CMQTTSUB: (0-1),(1-1024),(0-2),(0-1)
        thisModem().sendAT("+CMQTTSUB=", clientIndex, ',', strlen(topic), ',', qos, ',', dup);
        if (thisModem().waitResponse(10000UL, ">") != 1) {
            return false;
        }
        thisModem().waitResponse('>');
        thisModem().stream.write(topic);
        thisModem().stream.println();
        // Wait return OK
        if (thisModem().waitResponse(10000UL) != 1) {
            return false;
        }
        // SUBACK(+CMQTTSUB)은 "OK" 와 달리 브로커까지 왕복한 뒤에 온다.
        // 인자 없는 waitResponse는 기본 타임아웃이 1000ms 뿐이라 TLS+LTE 에서는 쉽게 넘긴다.
        // 타임아웃이 나면 아래 streamGetIntBefore가 엉뚱한 값을 읽어, 실제로는 성공한
        // 구독을 실패로 오판한다(반대로 브로커 거부도 같은 false 라 구분이 안 된다).
        if (thisModem().waitResponse(10000UL, "+CMQTTSUB: ") != 1) {
            DBG("### CMQTTSUB: SUBACK 타임아웃");
            return false;
        }
        int id = thisModem().streamGetIntBefore(',');
        int status = thisModem().streamGetIntBefore('\n');
        thisModem().stream.flush();
        if (status != 0) {
            DBG("### CMQTTSUB: 브로커 거부 err=", status);
        }
        return id == clientIndex && status == 0;
    }

    bool mqtt_unsubscribe(uint8_t clientIndex, const char *topic)
    {
        if (clientIndex > muxCount) {
            return false;
        }
        thisModem().sendAT("+CMQTTUNSUBTOPIC=", clientIndex, ',', strlen(topic));
        if (thisModem().waitResponse(10000UL, ">") != 1) {
            return false;
        }
        thisModem().stream.write(topic);
        thisModem().stream.println();
        // Wait return OK
        if (thisModem().waitResponse(10000UL) != 1) {
            return false;
        }
        // Subscribe a message to server
        // +CMQTTSUB: (0-1),(1-1024),(0-2),(0-1)
        thisModem().sendAT("+CMQTTUNSUB=", clientIndex, ',', '0');
        if (thisModem().waitResponse(10000UL) != 1) {
            return false;
        }
        // thisModem().waitResponse("+CMQTTUNSUB: 0,0");
        // return true;
        thisModem().waitResponse("+CMQTTUNSUB: ");
        int id = thisModem().streamGetIntBefore(',');
        int status = thisModem().streamGetIntBefore('\n');
        thisModem().stream.flush();
        return id == clientIndex && status == 0;
    }

    bool mqtt_connected(uint8_t clientIndex = 0)
    {
        if (clientIndex > muxCount) {
            return false;
        }
        if (millis() - _lastCheckConnect < 10000) {
            return _isConnected;
        }
        _lastCheckConnect = millis();
        int result = 0;
        int i = TINY_GSM_MQTT_CLI_COUNT;
        thisModem().sendAT("+CMQTTDISC?");
        while (i--) {
            if (thisModem().waitResponse("+CMQTTDISC: ") == 1) {
                if (thisModem().streamGetIntBefore(',') == clientIndex) {
                    result = thisModem().streamGetIntBefore('\n');
                    thisModem().waitResponse();
                    _isConnected =  (result == 0);
                    return _isConnected;
                }
            }
        }
        _isConnected = false;
        return false;
    }




    bool mqtt_set_rx_buffer_size(uint32_t size)
    {
        if (size == 0) {
            return false;
        }
        if (size == this->bufferSize) {
            return true;
        }
        if (this->bufferSize == 0) {
            this->buffer = (uint8_t *)TINY_GSM_MALLOC(size);
        } else {
            uint8_t *newBuffer = (uint8_t *)TINY_GSM_REALLOC(this->buffer, size);
            if (newBuffer != NULL) {
                this->buffer = newBuffer;
            } else {
                return false;
            }
        }
        this->bufferSize = size;
        return (this->buffer != NULL);
    }



    void mqtt_set_callback(callback_t cb)
    {
        this->callback = cb;
    }
    /*

    +CMQTTRXSTART: 0,17,2039
    Recvice CMQTTRXSTART
    topic_total_len:17
    payload_total_len:2039
    +CMQTTRXTOPIC: 0,17
    /Sim7600/user/get
    +CMQTTRXPAYLOAD: 0,1500
        +CMQTTRXEND: 0
    */
    // +CMQTTRXSTART:<client_index>,<topic_total_len>,<payload_total_len>
    // +CMQTTRXTOPIC: <client_index>, <sub_topic_len><sub_topic>
    bool mqtt_handle(uint32_t timeout = 100)
    {
        //TODO:More than 1500 bytes will carry the Modem return flag
        // +CMQTTCONNLOST: 1,1
        if (thisModem().waitResponse(timeout, "+CMQTTRXSTART:") == 1) {
            thisModem().streamSkipUntil(',');
            size_t topicSize = 0;
            size_t plyloadSize = 0;
            size_t topic_total_len =  thisModem().streamGetIntBefore(',');
            size_t payload_total_len =  thisModem().streamGetIntBefore('\n');
            if (thisModem().waitResponse(timeout, "+CMQTTRXTOPIC:") == 1) {

                thisModem().streamSkipUntil('\n');
                topicSize = topic_total_len > bufferSize ? bufferSize - 1 : topic_total_len;
                thisModem().stream.readBytes(buffer, topicSize);
                buffer[topicSize] = '\0';
                topicSize += 1;

                if (topicSize == bufferSize) {
                    DBG("Buffer overflow!");
                    thisModem().waitResponse(10000UL);
                    return false;
                }
                size_t recvSize = 0;
                size_t remainingSize = bufferSize - topicSize;
                size_t bufferOffset = topicSize;

                do {
                    if (thisModem().waitResponse(timeout, "+CMQTTRXPAYLOAD:") == 1) {
                        thisModem().streamSkipUntil(',');
                        int packetSize = thisModem().streamGetIntBefore('\n');

                        plyloadSize = packetSize > remainingSize ? remainingSize : packetSize;

                        if (bufferOffset >= bufferSize) {
                            DBG("Buffer overflow!");
                            break;
                        }
                        thisModem().stream.readBytes(buffer + bufferOffset, plyloadSize);

                        remainingSize -= plyloadSize;
                        bufferOffset += plyloadSize;
                        recvSize += packetSize;

                    } else {
                        // URC 가 끊기면 recvSize 가 영영 payload_total_len 에 닿지 못해
                        // do-while 이 무한 루프가 된다(→ 워치독 리셋). 끊기면 중단한다.
                        DBG("### CMQTTRXPAYLOAD 누락 — 수신 중단");
                        break;
                    }
                } while (recvSize != payload_total_len);

                if (thisModem().waitResponse(timeout, "+CMQTTRXEND: 0") == 1) {
                    if (this->callback) {
                        this->callback((const char *)buffer, buffer + topicSize, recvSize);
                    }
                    memset(this->buffer, 0, bufferSize);
                    return true;
                }

            }
        }
        return false;
    }


protected:
    bool mqttWillTopic(uint8_t clientIndex, const char *topic)
    {
        if (clientIndex > muxCount) {
            DBG("Error: Client index out of bounds");
            return false;
        }

        // Set the Will topic
        // +CMQTTWILLTOPIC: <client_index>,<req_length>
        thisModem().sendAT("+CMQTTWILLTOPIC=", clientIndex, ',', strlen(topic));

        int response = thisModem().waitResponse(10000UL, ">");
        if (response != 1) {
            DBG("Error: Did not receive expected '>' prompt, response: ", response);
            return false;
        }

        // Send the actual topic
        thisModem().stream.write(topic);
        thisModem().stream.println();

        response = thisModem().waitResponse();
        if (response != 1) {
            DBG("Error: Did not receive 'OK' after sending the Will topic, response: ", response);
            return false;
        }

        return true;
    }

    bool mqttWillMessage(uint8_t clientIndex, const char *message, uint8_t qos)
    {
        if (clientIndex > muxCount) {
            DBG("Error: Client index out of bounds");
            return false;
        }

        // Set the Will message
        // +CMQTTWILLMSG: <client_index>,<req_length>,<qos>
        thisModem().sendAT("+CMQTTWILLMSG=", clientIndex, ',', strlen(message), ',', qos);

        int response = thisModem().waitResponse(10000UL, ">");
        if (response != 1) {
            DBG("Error: Did not receive expected '>' prompt, response: ", response);
            return false;
        }

        // Send the actual message
        thisModem().stream.write(message);
        thisModem().stream.println();

        response = thisModem().waitResponse();
        if (response != 1) {
            DBG("Error: Did not receive 'OK' after sending the Will message, response: ", response);
            return false;
        }

        return true;
    }
    /*
     * CRTP Helper
     */
protected:
    inline const modemType &thisModem() const
    {
        return static_cast<const modemType &>(*this);
    }
    inline modemType &thisModem()
    {
        return static_cast<modemType &>(*this);
    }
};



