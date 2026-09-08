resource "google_artifact_registry_repository_iam_member" "zedis_vm_reader" {
  project    = "zedisv1"
  location   = "asia-south1"
  repository = "zedis"

  role   = "roles/artifactregistry.reader"
  member = "serviceAccount:${google_service_account.zedis_vm.email}"
}
