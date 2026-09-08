resource "google_service_account" "zedis_vm" {
  account_id   = "zedis-vm"
  display_name = "Zedis VM service account"
}
