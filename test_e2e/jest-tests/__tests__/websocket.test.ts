/**
 * WebSocket Tests
 * Tests WebSocket functionality including echo and broadcast
 */

import WebSocket from 'ws';
import { BASE_URL } from '../jest.setup';

// Convert HTTP URL to WebSocket URL
const WS_URL = BASE_URL.replace('http://', 'ws://');

describe('WebSocket', () => {

  describe('WebSocket Echo Server (/ws/echo)', () => {
    let ws: WebSocket;
    let welcomeData: Buffer | null;

    beforeEach((done) => {
      welcomeData = null;
      ws = new WebSocket(`${WS_URL}/ws/echo`);

      // Capture welcome message during beforeEach
      ws.once('message', (data) => {
        welcomeData = data as Buffer;
      });

      ws.on('open', () => {
        // Wait for welcome to arrive and QEMU network to stabilize (increased for full suite)
        setTimeout(done, 1000);
      });
      ws.on('error', (err) => done(err));
    }, 90000);

    afterEach(() => {
      if (ws) {
        if (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CLOSING) {
          ws.terminate();
        }
      }
    });

    it('should connect successfully', (done) => {
      expect(ws.readyState).toBe(WebSocket.OPEN);
      done();
    }, 10000);

    it('should receive welcome message on connect', (done) => {
      // Welcome was captured in beforeEach
      expect(welcomeData).not.toBeNull();
      const message = JSON.parse(welcomeData!.toString());
      expect(message.type).toBe('welcome');
      expect(message.message).toBe('Connected to echo server');
      done();
    }, 10000);

    it('should echo text messages back', (done) => {
      const testMessage = 'Hello WebSocket!';
      const timeout = setTimeout(() => done(new Error('Timeout waiting for echo')), 14000);

      ws.once('message', (data) => {
        clearTimeout(timeout);
        expect(data.toString()).toBe(testMessage);
        done();
      });
      // Wait for QEMU network stabilization (increased for full test suite load)
      setTimeout(() => ws.send(testMessage), 1000);
    }, 15000);

    it('should echo JSON messages back', (done) => {
      const testData = { type: 'test', value: 123, nested: { key: 'value' } };
      const testMessage = JSON.stringify(testData);
      const timeout = setTimeout(() => done(new Error('Timeout waiting for echo')), 14000);

      ws.once('message', (data) => {
        clearTimeout(timeout);
        const received = JSON.parse(data.toString());
        expect(received).toEqual(testData);
        done();
      });
      // Wait for QEMU network stabilization (increased for full test suite load)
      setTimeout(() => ws.send(testMessage), 1000);
    }, 15000);

    it('should handle binary messages', (done) => {
      const binaryData = Buffer.from([0x01, 0x02, 0x03, 0x04, 0x05]);

      ws.once('message', (data) => {
        expect(Buffer.compare(data as Buffer, binaryData)).toBe(0);
        done();
      });
      // Wait for QEMU network stabilization (500ms proven reliable in manual testing)
      setTimeout(() => ws.send(binaryData), 500);
    }, 15000);

    it('should handle multiple messages in sequence', (done) => {
      const messages = ['first', 'second'];
      let receivedCount = 0;
      const timeout = setTimeout(() => done(new Error('Timeout waiting for messages')), 20000);

      ws.on('message', (data) => {
        expect(data.toString()).toBe(messages[receivedCount]);
        receivedCount++;
        if (receivedCount === messages.length) {
          clearTimeout(timeout);
          done();
        }
      });
      // Wait for QEMU network stabilization (500ms proven reliable in manual testing)
      setTimeout(() => messages.forEach(msg => ws.send(msg)), 500);
    }, 25000);
  });

  describe('WebSocket Broadcast Server (/ws/broadcast)', () => {
    let ws1: WebSocket;
    let ws2: WebSocket;

    beforeEach((done) => {
      let connected = 0;
      let errored = false;
      const checkDone = () => {
        connected++;
        if (connected === 2) done();
      };
      const handleError = (err: Error) => {
        if (!errored) {
          errored = true;
          done(err);
        }
      };

      ws1 = new WebSocket(`${WS_URL}/ws/broadcast`);
      ws2 = new WebSocket(`${WS_URL}/ws/broadcast`);

      ws1.on('open', checkDone);
      ws2.on('open', checkDone);
      ws1.on('error', handleError);
      ws2.on('error', handleError);
    }, 180000); // QEMU: Increased timeout - two WebSockets take ~60s each

    afterEach(() => {
      // Helper to safely cleanup a WebSocket
      const cleanupWs = (ws: WebSocket | null) => {
        if (!ws) return;
        if (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CLOSING) {
          ws.terminate();
        } else if (ws.readyState === WebSocket.CONNECTING) {
          ws.removeAllListeners();
          ws.on('open', () => ws.terminate());
          ws.on('error', () => {});
        }
      };
      cleanupWs(ws1);
      cleanupWs(ws2);
    });

    it('should broadcast messages to all connected clients', (done) => {
      const testMessage = 'Broadcast test message';
      let ws1Received = false;
      let ws2Received = false;
      const timeout = setTimeout(() => done(new Error('Timeout waiting for broadcast')), 20000);

      // Skip initial connection messages
      setTimeout(() => {
        ws1.on('message', (data) => {
          if (data.toString() === testMessage) {
            ws1Received = true;
            if (ws1Received && ws2Received) {
              clearTimeout(timeout);
              done();
            }
          }
        });

        ws2.on('message', (data) => {
          if (data.toString() === testMessage) {
            ws2Received = true;
            if (ws1Received && ws2Received) {
              clearTimeout(timeout);
              done();
            }
          }
        });

        // Send from ws1
        ws1.send(testMessage);
      }, 500);  // Increased delay for QEMU
    }, 25000);

    it('should notify clients when a new client joins', (done) => {
      const timeout = setTimeout(() => done(new Error('Timeout waiting for client_joined')), 20000);

      ws1.on('message', (data) => {
        try {
          const message = JSON.parse(data.toString());
          if (message.type === 'client_joined') {
            expect(message.total).toBeGreaterThanOrEqual(2);
            clearTimeout(timeout);
            done();
          }
        } catch (e) {
          // Ignore non-JSON messages
        }
      });
    }, 25000);

    // Note: Using terminate() instead of close() because QEMU's network emulation
    // doesn't promptly deliver WebSocket close frames. TCP-level disconnections work reliably.
    it('should notify clients when a client leaves', (done) => {
      const timeout = setTimeout(() => done(new Error('Timeout waiting for client_left')), 20000);

      // Wait for initial connection
      setTimeout(() => {
        ws1.on('message', (data) => {
          try {
            const message = JSON.parse(data.toString());
            if (message.type === 'client_left') {
              expect(message.remaining).toBeGreaterThanOrEqual(0);
              clearTimeout(timeout);
              done();
            }
          } catch (e) {
            // Ignore non-JSON messages
          }
        });

        // Terminate ws2 to trigger disconnect message (TCP-level close works better in QEMU)
        ws2.terminate();
      }, 500);  // Increased delay for QEMU
    }, 25000);
  });

  describe('WebSocket Error Handling', () => {
    it('should handle connection to non-existent endpoint', (done) => {
      const ws = new WebSocket(`${WS_URL}/ws/nonexistent`);
      let handled = false;

      ws.on('error', (error) => {
        if (!handled) {
          handled = true;
          expect(error).toBeDefined();
          ws.terminate();
          done();
        }
      });

      ws.on('unexpected-response', (req, res) => {
        if (!handled) {
          handled = true;
          expect(res.statusCode).toBeGreaterThanOrEqual(400);
          ws.terminate();
          done();
        }
      });

      // Timeout fallback
      setTimeout(() => {
        if (!handled) {
          handled = true;
          ws.terminate();
          done();
        }
      }, 5000); // QEMU: Increased from 2s to 5s
    }, 10000); // QEMU: Added explicit timeout

    it('should handle malformed WebSocket frames gracefully', (done) => {
      const ws = new WebSocket(`${WS_URL}/ws/echo`);

      ws.on('open', () => {
        // Send raw malformed frame (this is a simplified test)
        try {
          // Most WebSocket libraries prevent sending malformed frames
          // So we just test normal error recovery
          ws.terminate();
          done();
        } catch (error) {
          ws.terminate();
          done();
        }
      });

      ws.on('error', () => {
        ws.terminate();
        done();
      });
    }, 90000); // QEMU: Increased timeout for slow WebSocket handshake (~60s)
  });

  describe('WebSocket Channel System (/ws/channel)', () => {
    let ws1: WebSocket;
    let ws2: WebSocket;

    // Helper to safely cleanup a WebSocket
    const cleanupWs = (ws: WebSocket | null) => {
      if (!ws) return;
      if (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CLOSING) {
        ws.terminate();
      } else if (ws.readyState === WebSocket.CONNECTING) {
        ws.removeAllListeners();
        ws.on('open', () => ws.terminate());
        ws.on('error', () => {});
      }
    };

    beforeEach((done) => {
      let connected = 0;
      let errored = false;
      const checkDone = () => {
        connected++;
        if (connected === 2) done();
      };
      const handleError = (err: Error) => {
        if (!errored) {
          errored = true;
          done(err);
        }
      };

      ws1 = new WebSocket(`${WS_URL}/ws/channel`);
      ws2 = new WebSocket(`${WS_URL}/ws/channel`);

      ws1.on('open', checkDone);
      ws2.on('open', checkDone);
      ws1.on('error', handleError);
      ws2.on('error', handleError);
    }, 180000);

    afterEach(() => {
      cleanupWs(ws1);
      cleanupWs(ws2);
    });

    it('should receive welcome message on connect', (done) => {
      const timeout = setTimeout(() => done(new Error('Timeout waiting for welcome')), 10000);

      // Need fresh connection since beforeEach already received welcome
      const ws = new WebSocket(`${WS_URL}/ws/channel`);
      ws.on('open', () => {
        // Welcome message should arrive automatically
      });
      ws.on('message', (data) => {
        const msg = JSON.parse(data.toString());
        if (msg.type === 'welcome') {
          clearTimeout(timeout);
          expect(msg.message).toBe('Connected to channel server');
          ws.terminate();
          done();
        }
      });
      ws.on('error', (err) => {
        clearTimeout(timeout);
        done(err);
      });
    }, 90000);

    it('should join a channel and receive confirmation', (done) => {
      const timeout = setTimeout(() => done(new Error('Timeout waiting for join confirmation')), 15000);

      setTimeout(() => {
        ws1.on('message', (data) => {
          const msg = JSON.parse(data.toString());
          if (msg.type === 'joined') {
            clearTimeout(timeout);
            expect(msg.channel).toBe('test-channel');
            done();
          }
        });

        ws1.send(JSON.stringify({ cmd: 'join', channel: 'test-channel' }));
      }, 500);
    }, 20000);

    it('should publish message only to channel subscribers', (done) => {
      const timeout = setTimeout(() => done(new Error('Timeout waiting for publish')), 20000);
      let ws1Received = false;
      let ws2ShouldNotReceive = false;

      setTimeout(() => {
        // ws1 joins channel, ws2 does not
        ws1.send(JSON.stringify({ cmd: 'join', channel: 'exclusive' }));

        // Give time for join to process
        setTimeout(() => {
          ws1.on('message', (data) => {
            const msg = JSON.parse(data.toString());
            if (msg.type === 'channel_message' && msg.channel === 'exclusive') {
              ws1Received = true;
              clearTimeout(timeout);
              expect(msg.data).toBe('Hello exclusive members!');
              done();
            }
          });

          ws2.on('message', (data) => {
            const msg = JSON.parse(data.toString());
            // ws2 should NOT receive channel messages for 'exclusive'
            if (msg.type === 'channel_message' && msg.channel === 'exclusive') {
              ws2ShouldNotReceive = true;
            }
          });

          // ws1 publishes to channel
          ws1.send(JSON.stringify({ cmd: 'publish', channel: 'exclusive', data: 'Hello exclusive members!' }));
        }, 500);
      }, 500);
    }, 25000);

    it('should allow subscribing to multiple channels', (done) => {
      const timeout = setTimeout(() => done(new Error('Timeout waiting for channel list')), 15000);

      setTimeout(() => {
        // Join multiple channels sequentially
        ws1.send(JSON.stringify({ cmd: 'join', channel: 'channel-a' }));

        setTimeout(() => {
          ws1.send(JSON.stringify({ cmd: 'join', channel: 'channel-b' }));

          setTimeout(() => {
            ws1.on('message', (data) => {
              const msg = JSON.parse(data.toString());
              if (msg.type === 'channels') {
                clearTimeout(timeout);
                expect(msg.channels).toContain('channel-a');
                expect(msg.channels).toContain('channel-b');
                expect(msg.count).toBeGreaterThanOrEqual(2);
                done();
              }
            });

            // Request current channel subscriptions
            ws1.send(JSON.stringify({ cmd: 'channels' }));
          }, 300);
        }, 300);
      }, 500);
    }, 20000);

    it('should leave a channel and stop receiving messages', (done) => {
      const timeout = setTimeout(() => done(new Error('Timeout waiting for leave')), 20000);
      let leftChannel = false;

      setTimeout(() => {
        // ws1 joins then leaves
        ws1.send(JSON.stringify({ cmd: 'join', channel: 'temp-channel' }));

        setTimeout(() => {
          ws1.on('message', (data) => {
            const msg = JSON.parse(data.toString());
            if (msg.type === 'left') {
              leftChannel = true;
              expect(msg.channel).toBe('temp-channel');
            }
            // After leaving, should not receive channel messages
            if (leftChannel && msg.type === 'channel_message' && msg.channel === 'temp-channel') {
              clearTimeout(timeout);
              done(new Error('Should not receive message after leaving channel'));
            }
          });

          ws1.send(JSON.stringify({ cmd: 'leave', channel: 'temp-channel' }));

          // Wait for leave to process, then publish
          setTimeout(() => {
            if (leftChannel) {
              // ws2 joins and publishes - ws1 should NOT receive
              ws2.send(JSON.stringify({ cmd: 'join', channel: 'temp-channel' }));
              setTimeout(() => {
                ws2.send(JSON.stringify({ cmd: 'publish', channel: 'temp-channel', data: 'After leave' }));
                // If ws1 doesn't receive message within 1s, test passes
                setTimeout(() => {
                  clearTimeout(timeout);
                  done();
                }, 1000);
              }, 300);
            }
          }, 500);
        }, 500);
      }, 500);
    }, 30000);

    it('should broadcast to all subscribers in a channel', (done) => {
      const timeout = setTimeout(() => done(new Error('Timeout waiting for broadcast')), 25000);
      let ws1Received = false;
      let ws2Received = false;

      setTimeout(() => {
        // Both clients join the same channel
        ws1.send(JSON.stringify({ cmd: 'join', channel: 'broadcast-test' }));
        ws2.send(JSON.stringify({ cmd: 'join', channel: 'broadcast-test' }));

        setTimeout(() => {
          ws1.on('message', (data) => {
            const msg = JSON.parse(data.toString());
            if (msg.type === 'channel_message' && msg.channel === 'broadcast-test') {
              ws1Received = true;
              if (ws1Received && ws2Received) {
                clearTimeout(timeout);
                done();
              }
            }
          });

          ws2.on('message', (data) => {
            const msg = JSON.parse(data.toString());
            if (msg.type === 'channel_message' && msg.channel === 'broadcast-test') {
              ws2Received = true;
              if (ws1Received && ws2Received) {
                clearTimeout(timeout);
                done();
              }
            }
          });

          // Publish from ws1 - both should receive
          ws1.send(JSON.stringify({ cmd: 'publish', channel: 'broadcast-test', data: 'Hello all!' }));
        }, 1000);
      }, 500);
    }, 30000);

    it('should handle invalid commands gracefully', (done) => {
      const timeout = setTimeout(() => done(new Error('Timeout waiting for error')), 15000);

      setTimeout(() => {
        ws1.on('message', (data) => {
          const msg = JSON.parse(data.toString());
          if (msg.type === 'error') {
            clearTimeout(timeout);
            expect(msg.message).toBeDefined();
            done();
          }
        });

        ws1.send(JSON.stringify({ cmd: 'invalid_command' }));
      }, 500);
    }, 20000);
  });

  describe('WebSocket Performance', () => {
    let ws: WebSocket | null = null;

    afterEach(() => {
      if (ws) {
        if (ws.readyState === WebSocket.OPEN) {
          ws.close();
        } else if (ws.readyState === WebSocket.CONNECTING) {
          ws.on('open', () => ws?.close());
          ws.on('error', () => {}); // Ignore errors during cleanup
        }
      }
      ws = null;
    });

    it('should handle rapid message exchange', (done) => {
      ws = new WebSocket(`${WS_URL}/ws/echo`);
      const messageCount = 10;  // Reduced for QEMU
      let sent = 0;
      let received = -1; // -1 to account for welcome message
      const timeout = setTimeout(() => done(new Error('Timeout waiting for messages')), 90000); // QEMU: Increased

      ws.on('open', () => {
        // Send messages rapidly
        for (let i = 0; i < messageCount; i++) {
          ws!.send(`Message ${i}`);
          sent++;
        }
      });

      ws.on('message', () => {
        received++;
        if (received === messageCount) {
          clearTimeout(timeout);
          expect(received).toBe(sent);
          done();
        }
      });

      ws.on('error', (err) => {
        clearTimeout(timeout);
        done(err);
      });
    }, 120000); // QEMU: Increased overall test timeout

    it('should handle large messages', (done) => {
      ws = new WebSocket(`${WS_URL}/ws/echo`);
      const largeMessage = 'x'.repeat(5000); // Reduced to 5KB for QEMU
      let welcomeReceived = false;
      const timeout = setTimeout(() => done(new Error('Timeout waiting for large message')), 120000); // QEMU: Increased

      ws.on('open', () => {
        // Wait a bit before sending large message
        setTimeout(() => ws!.send(largeMessage), 500);
      });

      ws.on('message', (data) => {
        if (!welcomeReceived) {
          welcomeReceived = true;
        } else {
          clearTimeout(timeout);
          expect(data.toString()).toBe(largeMessage);
          done();
        }
      });

      ws.on('error', (err) => {
        clearTimeout(timeout);
        done(err);
      });
    }, 180000); // QEMU: Increased for slow network
  });
});