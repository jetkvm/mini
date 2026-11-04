// The types of release_signature.mjs, for the e2e suite's mock update server.
export interface ReleaseKey {
  rootFingerprint: string;
  signingFingerprint: string;
  signingPrivateKey: string;
  publicKey: Buffer;
}
export function testReleaseKey(): ReleaseKey;
export function signDetached(
  data: Buffer,
  key: Pick<ReleaseKey, "signingFingerprint" | "signingPrivateKey">,
  createdSeconds?: number,
): Buffer;
