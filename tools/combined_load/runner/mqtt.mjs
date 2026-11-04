// mqtt: the Mini connected to an MQTT broker over TLS, as for a user of Home
// Assistant. The broker is the one of the integration tests
// (tools/integration/lib/mqtt_broker.mjs), on this machine. Its certificate
// is one that no client trusts, so the Mini connects without verification.
import { startMqttBroker } from '../../integration/lib/mqtt_broker.mjs';

/** The base topic that the switch gives the Mini; the Mini adds its device ID. */
export const BASE_TOPIC = 'combined-load';

/**
 * The switch of mqtt. rpc(method, params, timeoutMs): the Mini's JSON-RPC, by
 * which the settings page stores the MQTT settings. address: this machine's
 * address towards the Mini. stats(): the Mini's short stats, or null.
 *
 * On: the broker starts, and the Mini gets it as its broker, with TLS without
 * verification, BASE_TOPIC and Home Assistant discovery. The other settings
 * stay as the Mini has them; the Mini answers with a mask for a stored
 * password and keeps the password when it gets the mask back. Off: the Mini
 * gets the settings that it had before, and the broker closes.
 */
export function mqttSwitch({ rpc, address, stats, start = startMqttBroker }) {
  let broker = null;
  let before = null;
  let deviceId = null;
  let since = null;
  // The Mini's newest own message on a topic of its states. The broker also
  // publishes the Mini's last will there when the connection ends.
  const newest = suffix => broker?.messages(`${BASE_TOPIC}/${deviceId}/${suffix}`).findLast(message => message.origin === 'client');
  const connected = () => Boolean(broker?.subscriptions(`jetkvm-${deviceId}`));
  return {
    on: async () => {
      deviceId = await rpc('getDeviceID');
      broker = await start({ host: address, secure: true });
      before = await rpc('getMqttSettings');
      since = Date.now();
      await rpc('setMqttSettings', { settings: { ...before, enabled: true, broker: address, port: broker.port,
        base_topic: BASE_TOPIC, use_tls: true, tls_insecure: true, enable_ha_discovery: true } }, 30_000);
    },
    off: async () => {
      if (before) await rpc('setMqttSettings', { settings: before }, 30_000);
      before = null;
      since = null;
      await broker?.close();
      broker = null;
    },
    // The broker's view: it has the Mini's connection.
    isOn: connected,
    // On: the broker has the Mini's video state, and the Mini's short stats
    // have the video mode to compare it with.
    works: async on => {
      if (!on) return true;
      if (!newest('video/state')) return 'the broker has no video/state message of the Mini';
      return typeof (await stats())?.modeValid === 'boolean' || 'the Mini\'s short stats do not have its video mode (modeValid)';
    },
    // since: when the Mini got the broker, or null before that. connected:
    // the broker has the Mini's connection. statusAt: when the Mini's newest
    // status message came. video and videoAt: what its newest video/state
    // message says and when it came; a payload that is not a JSON object is
    // {}. The times are null without a message.
    sample: () => {
      const said = newest('video/state');
      let video = null;
      try {
        video = said ? Object(JSON.parse(said.payload)) : null;
      } catch {
        video = {};
      }
      return { since, connected: connected(), statusAt: newest('status')?.at ?? null, videoAt: said?.at ?? null, video };
    },
  };
}
