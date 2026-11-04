#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "jetkvm_ota.h"

static void test_parse_metadata_complete(void)
{
    const char *json =
        "{\"appVersion\":\"0.5.0-p4.2\",\"appUrl\":\"http://192.168.50.1/a.bin\","
        "\"appHash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
        "\"systemVersion\":\"89abcdef\",\"systemUrl\":\"http://192.168.50.1/w.bin\","
        "\"systemHash\":\"fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210\","
        "\"appSigUrl\":\"http://ignored.example/sig\","
        "\"systemSigUrl\":\"http://192.168.50.1/w.bin.sig\"}";
    jetkvm_ota_metadata_t metadata;
    assert(jetkvm_ota_parse_metadata(json, strlen(json), &metadata));
    assert(strcmp(metadata.app_version, "0.5.0-p4.2") == 0);
    assert(strcmp(metadata.app_url, "http://192.168.50.1/a.bin") == 0);
    assert(strcmp(metadata.app_hash,
                  "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef") == 0);
    assert(strcmp(metadata.system_version, "89abcdef") == 0);
    assert(strcmp(metadata.system_hash,
                  "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210") == 0);
    assert(strcmp(metadata.system_sig_url, "http://192.168.50.1/w.bin.sig") == 0);
}

static void test_parse_metadata_partial_offer_rejected(void)
{
    /* version+url but no hash: the downloader must never see it */
    const char *json =
        "{\"appVersion\":\"0.5.0-p4.2\",\"appUrl\":\"http://x/a.bin\"}";
    jetkvm_ota_metadata_t metadata;
    assert(!jetkvm_ota_parse_metadata(json, strlen(json), &metadata));
    assert(metadata.app_version[0] == '\0');
}

static void test_parse_metadata_empty_and_malformed(void)
{
    jetkvm_ota_metadata_t metadata;
    assert(jetkvm_ota_parse_metadata("{}", 2, &metadata));
    assert(metadata.app_version[0] == '\0');
    assert(metadata.system_url[0] == '\0');
    assert(!jetkvm_ota_parse_metadata("not json", 8, &metadata));
    assert(!jetkvm_ota_parse_metadata("[1,2]", 5, &metadata));
    assert(!jetkvm_ota_parse_metadata(NULL, 0, &metadata));

    /* only system offered is fine */
    const char *web_only =
        "{\"systemVersion\":\"00000001\",\"systemUrl\":\"http://x/w.bin\","
        "\"systemHash\":\"fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210\"}";
    assert(jetkvm_ota_parse_metadata(web_only, strlen(web_only), &metadata));
    assert(metadata.app_version[0] == '\0');
    assert(strcmp(metadata.system_version, "00000001") == 0);
    /* an image without a signature has no systemSigUrl */
    assert(metadata.system_sig_url[0] == '\0');

    /* overlong field is rejected, not truncated */
    char long_url[400];
    strcpy(long_url, "{\"appVersion\":\"v\",\"appUrl\":\"http://");
    for (int i = 0; i < 300; i++) {
        strcat(long_url, "a");
    }
    strcat(long_url, "\",\"appHash\":\"h\"}");
    assert(!jetkvm_ota_parse_metadata(long_url, strlen(long_url), &metadata));
}

static void test_update_available(void)
{
    /* no offer without both versions */
    assert(!jetkvm_ota_update_available("0.5.0-p4.1", "", NULL));
    assert(!jetkvm_ota_update_available(NULL, "0.5.4", NULL));
    assert(!jetkvm_ota_update_available("0.5.4", NULL, NULL));

    /* unconstrained checks follow semver precedence, like Go's
     * remoteVersion.GreaterThan(localVersion) */
    assert(!jetkvm_ota_update_available("0.5.4", "0.5.4", NULL));
    assert(jetkvm_ota_update_available("0.5.3", "0.5.4", NULL));
    assert(!jetkvm_ota_update_available("0.5.4", "0.5.3", NULL));
    assert(jetkvm_ota_update_available("0.5.4", "0.6.0", NULL));
    assert(jetkvm_ota_update_available("0.9.9", "1.0.0", NULL));
    assert(jetkvm_ota_update_available("0.5.0-p4.1", "0.5.0-p4.2", NULL));
    assert(!jetkvm_ota_update_available("0.5.0-p4.2", "0.5.0-p4.1", NULL));

    /* a dev build past the last release must not be offered a downgrade */
    assert(!jetkvm_ota_update_available("0.5.5-dev20260921", "0.5.4", NULL));

    /* a release outranks its own prereleases, and only those */
    assert(jetkvm_ota_update_available("0.5.5-dev20260921", "0.5.5", NULL));
    assert(!jetkvm_ota_update_available("0.5.5", "0.5.5-rc.1", NULL));

    /* the semver.org 2.0.0 prerelease ordering chain: numeric identifiers
     * before alphanumeric ones, then the longer identifier set */
    assert(jetkvm_ota_update_available("1.0.0-alpha", "1.0.0-alpha.1", NULL));
    assert(jetkvm_ota_update_available("1.0.0-alpha.1", "1.0.0-alpha.beta", NULL));
    assert(jetkvm_ota_update_available("1.0.0-alpha.beta", "1.0.0-beta", NULL));
    assert(jetkvm_ota_update_available("1.0.0-beta", "1.0.0-beta.2", NULL));
    assert(jetkvm_ota_update_available("1.0.0-beta.2", "1.0.0-beta.11", NULL));
    assert(jetkvm_ota_update_available("1.0.0-beta.11", "1.0.0-rc.1", NULL));
    assert(jetkvm_ota_update_available("1.0.0-rc.1", "1.0.0", NULL));
    assert(!jetkvm_ota_update_available("1.0.0-beta.11", "1.0.0-beta.2", NULL));

    /* build metadata never participates; a leading v and a short core
     * parse the way Masterminds semver accepts them */
    assert(!jetkvm_ota_update_available("0.5.4+build7", "0.5.4+build9", NULL));
    assert(jetkvm_ota_update_available("v0.5.4", "0.5.5", NULL));
    assert(jetkvm_ota_update_available("0.5", "0.5.1", NULL));
    assert(jetkvm_ota_update_available("0.5.4", "0.6.0+build.7", NULL));
    assert(jetkvm_ota_update_available("0.5.5-rc.1+sha", "0.5.5", NULL));

    /* malformed metadata fails the parse outright, like Masterminds */
    assert(!jetkvm_ota_update_available("0.5.4", "0.6.0+", NULL));
    assert(!jetkvm_ota_update_available("0.5.4", "0.6.0+bad!", NULL));
    assert(!jetkvm_ota_update_available("0.5.4", "0.6.0+a..b", NULL));
    assert(!jetkvm_ota_update_available("0.5.4", "0.6.0+meta.", NULL));
    assert(!jetkvm_ota_update_available("0.5.4+", "0.6.0", NULL));

    /* an unparseable version on either side never produces an offer */
    assert(!jetkvm_ota_update_available("unknown", "0.5.4", NULL));
    assert(!jetkvm_ota_update_available("0.5.4", "89abcdef", NULL));
    assert(!jetkvm_ota_update_available("0.5.4", "0.5.4.1", NULL));
    assert(!jetkvm_ota_update_available("0.5.4", "0.5.5-", NULL));

    /* a pinned version keeps Go's custom-version inequality, downgrades
     * included; an empty constraint means unconstrained */
    assert(jetkvm_ota_update_available("0.5.5-dev20260921", "0.5.4", "0.5.4"));
    assert(jetkvm_ota_update_available("0.5.0-p4.2", "0.5.0-p4.1", "0.5.0-p4.1"));
    assert(!jetkvm_ota_update_available("0.5.4", "0.5.4", "0.5.4"));
    assert(!jetkvm_ota_update_available("0.5.4", "", "0.5.4"));
    assert(!jetkvm_ota_update_available("0.5.4", "0.5.3", ""));
}

static void test_render_status_json(void)
{
    jetkvm_ota_update_status_t status;
    memset(&status, 0, sizeof(status));
    strcpy(status.local_app_version, "0.5.0-p4.1");
    strcpy(status.local_system_version, "89abcdef");
    strcpy(status.remote.app_version, "0.5.0-p4.2");
    status.app_update_available = true;
    char output[1024];
    size_t size = 0;
    assert(jetkvm_ota_render_status_json(&status, output, sizeof(output), &size));
    assert(strstr(output, "\"appVersion\":\"0.5.0-p4.2\"") != NULL);
    assert(strstr(output, "\"appUpdateAvailable\":true") != NULL);
    assert(strstr(output, "\"systemUpdateAvailable\":false") != NULL);
    assert(strstr(output, "\"willDisableAutoUpdate\":false") != NULL);
    assert(strstr(output, "\"error\"") == NULL);

    /* tiny buffer fails cleanly */
    char tiny[16];
    size_t tiny_size = 0;
    assert(!jetkvm_ota_render_status_json(&status, tiny, sizeof(tiny),
                                          &tiny_size));

    strcpy(status.error, "could not reach the update server");
    assert(jetkvm_ota_render_status_json(&status, output, sizeof(output), &size));
    assert(strstr(output, "\"error\":\"could not reach the update server\"") != NULL);
}

static void test_render_state_event(void)
{
    jetkvm_ota_state_t state;
    memset(&state, 0, sizeof(state));
    state.updating = true;
    state.metadata_fetched = true;
    state.app_update_pending = true;
    state.app_download_progress = 0.5f;
    char output[2048];
    size_t size = 0;
    assert(jetkvm_ota_render_state_event(&state, "2026-07-17T01:02:03Z",
                                         output, sizeof(output), &size));
    assert(strstr(output, "\"jsonrpc\":\"2.0\"") != NULL);
    assert(strstr(output, "\"method\":\"otaState\"") != NULL);
    assert(strstr(output, "\"updating\":true") != NULL);
    assert(strstr(output, "\"metadataFetchedAt\":\"2026-07-17T01:02:03Z\"") != NULL);
    assert(strstr(output, "\"appDownloadProgress\":0.5") != NULL);
    assert(strstr(output, "\"appDownloadFinishedAt\":null") != NULL);
    assert(strstr(output, "\"appVerifiedAt\":null") != NULL);
    assert(strstr(output, "\"systemUpdatePending\":false") != NULL);
    assert(strstr(output, "\"error\":null") != NULL);

    state.app_download_finished = true;
    state.app_verified = true;
    strcpy(state.error, "app image download or install failed");
    assert(jetkvm_ota_render_state_event(&state, "2026-07-17T01:02:04Z",
                                         output, sizeof(output), &size));
    assert(strstr(output, "\"appDownloadFinishedAt\":\"2026-07-17T01:02:04Z\"") != NULL);
    assert(strstr(output, "\"error\":\"app image download or install failed\"") != NULL);
}

static void test_validate_api_url(void)
{
    assert(jetkvm_ota_validate_api_url("http://192.168.50.1:8765"));
    assert(jetkvm_ota_validate_api_url("https://updates.example.com"));
    assert(jetkvm_ota_validate_api_url("http://192.168.50.1/"));
    assert(!jetkvm_ota_validate_api_url(""));
    assert(!jetkvm_ota_validate_api_url(NULL));
    assert(!jetkvm_ota_validate_api_url("ftp://192.168.50.1"));
    assert(!jetkvm_ota_validate_api_url("http://"));
    assert(!jetkvm_ota_validate_api_url("http://?x=1"));
    assert(!jetkvm_ota_validate_api_url("http://user@host/"));
    assert(!jetkvm_ota_validate_api_url("http://host/path?q=1"));
    char too_long[256];
    memset(too_long, 'a', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    memcpy(too_long, "http://", 7);
    assert(!jetkvm_ota_validate_api_url(too_long));
}

static void test_build_releases_url(void)
{
    char url[JETKVM_OTA_RELEASES_URL_CAPACITY];
    assert(jetkvm_ota_build_releases_url(
        url, sizeof(url), "http://192.168.50.1:8765", "e8f60ae2ce70",
        "jetkvm-mini-wireless", false, NULL, NULL));
    assert(strcmp(url, "http://192.168.50.1:8765/releases?deviceId=e8f60ae2ce70&prerelease=false&sku=jetkvm-mini-wireless") == 0);

    assert(jetkvm_ota_build_releases_url(
        url, sizeof(url), "http://192.168.50.1:8765/", "dev",
        "jetkvm-mini-ethernet", true, NULL, "0.5.0-p4.2"));
    assert(strcmp(url, "http://192.168.50.1:8765/releases?deviceId=dev&prerelease=true&sku=jetkvm-mini-ethernet&systemVersion=0.5.0-p4.2") == 0);

    assert(jetkvm_ota_build_releases_url(
        url, sizeof(url), "https://updates.example.com", "dev",
        "jetkvm-mini-wireless", false, "1.0.0", "1.0.1"));
    assert(strcmp(url, "https://updates.example.com/releases?deviceId=dev&prerelease=false&sku=jetkvm-mini-wireless&appVersion=1.0.0&systemVersion=1.0.1") == 0);

    /* invalid base, missing identity, missing SKU or tiny buffer rejected */
    assert(!jetkvm_ota_build_releases_url(url, sizeof(url), "ftp://x", "dev",
                                          "jetkvm-mini-wireless", false, NULL,
                                          NULL));
    char small[32];
    assert(!jetkvm_ota_build_releases_url(small, sizeof(small),
                                          "http://192.168.50.1:8765",
                                          "e8f60ae2ce70",
                                          "jetkvm-mini-wireless", false, NULL,
                                          NULL));
    assert(!jetkvm_ota_build_releases_url(url, sizeof(url),
                                          "http://192.168.50.1", "",
                                          "jetkvm-mini-wireless", false, NULL,
                                          NULL));
    assert(!jetkvm_ota_build_releases_url(url, sizeof(url),
                                          "http://192.168.50.1", "dev", "",
                                          false, NULL, NULL));
}

static void test_image_desc_matches(void)
{
    jetkvm_ota_image_desc_t desc = {
        .magic = JETKVM_OTA_IMAGE_DESC_MAGIC,
        .sku = "jetkvm-mini-wireless",
    };
    assert(jetkvm_ota_image_desc_matches(&desc, "jetkvm-mini-wireless"));
    assert(!jetkvm_ota_image_desc_matches(&desc, "jetkvm-mini-ethernet"));

    /* an image from before the marker: whatever bytes follow the app
     * descriptor, erased flash included */
    jetkvm_ota_image_desc_t unmarked;
    memset(&unmarked, 0xff, sizeof(unmarked));
    assert(!jetkvm_ota_image_desc_matches(&unmarked, "jetkvm-mini-wireless"));

    /* right magic, SKU field not terminated: never compared as a string */
    jetkvm_ota_image_desc_t unterminated = { .magic = JETKVM_OTA_IMAGE_DESC_MAGIC };
    memset(unterminated.sku, 'x', sizeof(unterminated.sku));
    assert(!jetkvm_ota_image_desc_matches(&unterminated, "jetkvm-mini-wireless"));

    assert(!jetkvm_ota_image_desc_matches(NULL, "jetkvm-mini-wireless"));
    assert(!jetkvm_ota_image_desc_matches(&desc, NULL));
}

int main(void)
{
    test_parse_metadata_complete();
    test_parse_metadata_partial_offer_rejected();
    test_parse_metadata_empty_and_malformed();
    test_update_available();
    test_render_status_json();
    test_render_state_event();
    test_validate_api_url();
    test_build_releases_url();
    test_image_desc_matches();
    puts("test_ota_core: all assertions passed");
    return 0;
}
