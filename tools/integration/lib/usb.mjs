// USB setup for tests that read the Mini's devices on the host.
import assert from 'node:assert/strict';
import { until } from './mini.mjs';

const read = async rpc => ({
  config: await rpc('getUsbConfig'),
  devices: await rpc('getUsbDevices'),
  enabled: await rpc('getUsbEmulationState'),
  audio: await rpc('getAudioConfig'),
  media: await rpc('getVirtualMediaState'),
});

/**
 * USB on with default IDs (399b:0001), keyboard, both mice and drive, nothing
 * mounted. The audio device stays as found; `audio: true` adds it and turns
 * the sound to the browser off. Every setting it changes is the Mini's
 * default, so a run that ends before restore() leaves no setting that a
 * later run refuses. Returns restore().
 */
export async function hostTestUsb(mini, { audio = false } = {}) {
  const { rpc } = mini;
  const found = await read(rpc);
  const restore = async () => {
    await mini.open();
    const errors = [];
    const step = action => action().catch(error => errors.push(error));
    if (found.audio) await step(() => rpc('setAudioConfig', { params: { enabled: false } }));
    await step(() => rpc('unmountImage'));
    await step(() => rpc('setUsbDevices', { devices: found.devices }));
    await step(() => rpc('setUsbConfig', { usbConfig: found.config }));
    await step(async () => {
      if (!found.media) return;
      if (found.media.source === 'HTTP' && found.media.url) await rpc('mountWithHTTP', { url: found.media.url, mode: found.media.mode });
      else if (found.media.source === 'Builtin') await rpc('mountBuiltinInstaller');
      else if (found.media.filename) await rpc('mountWithStorage', { filename: found.media.filename, mode: found.media.mode });
      else throw new Error(`no way to mount the image of the source ${found.media.source} again`);
    });
    if (found.audio) await step(() => rpc('setAudioConfig', { params: found.audio }));
    // Do not repeat setUsbEmulationState with the current state.
    await step(async () => {
      if ((await rpc('getUsbEmulationState')) !== found.enabled) await rpc('setUsbEmulationState', { enabled: found.enabled });
    });
    await step(async () => assert.deepEqual(await read(rpc), found, 'the Mini has the USB state that the test found'));
    if (errors.length) throw new AggregateError(errors, `the USB state of the Mini was not stored again: ${errors.map(e => e.message).join('; ')}`);
  };
  try {
    if (audio && found.audio?.enabled) await rpc('setAudioConfig', { params: { enabled: false } });
    if (found.media) await rpc('unmountImage');
    if (!found.enabled) await rpc('setUsbEmulationState', { enabled: true });
    const config = {
      vendor_id: '0x399b',
      product_id: '0x0001',
      serial_number: found.config.serial_number,
      manufacturer: 'JetKVM',
      product: 'USB Emulation Device',
    };
    await rpc('setUsbConfig', { usbConfig: config });
    await rpc('setUsbDevices', {
      devices: { ...found.devices, keyboard: true, absolute_mouse: true, relative_mouse: true, mass_storage: true, ...(audio ? { audio } : {}) },
    });
    const now = await until(() => rpc('getUsbConfig'), () => true, 60);
    for (const [name, value] of Object.entries(config)) assert.equal(now[name], value, `the Mini's USB ${name}`);
  } catch (error) {
    await restore().catch(() => undefined);
    throw error;
  }
  return restore;
}
