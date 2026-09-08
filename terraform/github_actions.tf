resource "google_iam_workload_identity_pool" "github" {
  project                   = "zedisv1"
  workload_identity_pool_id = "github"
  display_name              = "GitHub Actions"
  description               = "Workload Identity Federation for GitHub Actions"
}

resource "google_iam_workload_identity_pool_provider" "github" {
  project                            = "zedisv1"
  workload_identity_pool_id          = google_iam_workload_identity_pool.github.workload_identity_pool_id
  workload_identity_pool_provider_id = "github"

  display_name = "GitHub"

  attribute_mapping = {
    "google.subject"       = "assertion.sub"
    "attribute.actor"      = "assertion.actor"
    "attribute.repository" = "assertion.repository"
    "attribute.ref"        = "assertion.ref"
  }

  attribute_condition = "assertion.repository == 'BhagyeshKothalkar/zedis'"

  oidc {
    issuer_uri = "https://token.actions.githubusercontent.com"
  }
}

resource "google_service_account_iam_member" "github_ci" {
  service_account_id = google_service_account.zedis_ci.name

  role = "roles/iam.workloadIdentityUser"

  member = "principalSet://iam.googleapis.com/${google_iam_workload_identity_pool.github.name}/attribute.repository/BhagyeshKothalkar/zedis"
}
