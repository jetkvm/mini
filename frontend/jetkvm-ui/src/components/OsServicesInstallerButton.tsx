import LoadingSpinner from "@components/LoadingSpinner";
import { SplitButtonCaret, SplitButtonGroup, SplitButtonPrimary } from "@components/SplitButton";
import { useOsServicesInstaller, useOsServicesInstallerCard } from "@hooks/useOsServicesInstaller";
import { cx } from "@/cva.config";
import { m } from "@localizations/messages.js";

const Spinner = ({ className }: { className: string | undefined }) => (
  <LoadingSpinner className={cx(className, "animate-spin")} />
);

// "Mount installer" mounts the installer disc in the KVM's firmware at once;
// its menu saves the offline installer of `version`, or of the newest stable
// release, to the TF card first, for a target computer without internet.
export default function OsServicesInstallerButton({
  installer,
  version,
  theme,
  testId,
  menuTestId,
}: {
  installer: ReturnType<typeof useOsServicesInstaller>;
  version?: string;
  theme: "light" | "primary";
  testId: string;
  menuTestId: string;
}) {
  const cardReady = useOsServicesInstallerCard();
  return (
    <SplitButtonGroup>
      <SplitButtonPrimary
        theme={theme}
        size="SM"
        label={m.os_services_install_button()}
        icon={installer.mounting ? Spinner : undefined}
        disabled={installer.mounting}
        onClick={() => void installer.mount()}
        data-testid={testId}
      />
      <SplitButtonCaret
        theme={theme}
        size="SM"
        disabled={installer.mounting}
        data-testid={menuTestId}
        menuItems={[
          {
            label: m.os_services_install_offline(),
            description: cardReady === false ? m.os_services_install_no_card() : undefined,
            disabled: cardReady === false,
            onClick: () => void installer.mountOffline(version),
          },
        ]}
      />
    </SplitButtonGroup>
  );
}
