/** The address of this host on the route to the device, for a server that the device connects to. */
import net from "node:net";

export async function brokerHost(deviceUrl: string): Promise<string> {
  const url = new URL(deviceUrl);
  return new Promise((resolve, reject) => {
    const socket = net.connect({
      host: url.hostname.replace(/^\[|\]$/g, ""),
      port: Number(url.port || (url.protocol === "https:" ? 443 : 80)),
    });
    socket.setTimeout(5000, () => socket.destroy(new Error("Cannot find MQTT host address")));
    socket.once("error", reject);
    socket.once("connect", () => {
      const host = socket.localAddress!;
      socket.destroy();
      resolve(host);
    });
  });
}
