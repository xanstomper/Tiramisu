#include "../src/host/steam_client.h"

#include <cstdio>

int main() {
  longan::steam_library_item sideload;
  sideload.app_id = 101;
  sideload.installed = true;
  longan::steam_library_item owned;
  owned.app_id = 202;
  owned.account_owned = true;
  longan::steam_library_item owned_and_installed;
  owned_and_installed.app_id = 303;
  owned_and_installed.account_owned = true;
  owned_and_installed.installed = true;
  const auto ids = longan::steam_account_owned_appids({sideload, owned, owned_and_installed});
  if (ids != std::vector<uint32_t>{202, 303}) {
    std::fputs("Steam ownership must come from the account library.\n", stderr);
    return 1;
  }
  if (!longan::steam_account_owned_appids({sideload}).empty()) return 1;
  std::puts("Steam ownership checked independently of local installs");
  return 0;
}
