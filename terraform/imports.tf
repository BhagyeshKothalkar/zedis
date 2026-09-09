import {
  id = "projects/zedisv1/locations/asia-south1/repositories/zedis roles/artifactregistry.reader serviceAccount:zedis-vm@zedisv1.iam.gserviceaccount.com"
  to = google_artifact_registry_repository_iam_member.zedis_vm_reader
}

import {
  id = "projects/zedisv1/global/firewalls/allow-zedis"
  to = google_compute_firewall.zedis
}

import {
  id = "projects/zedisv1/zones/asia-south1-b/instances/zedis-vm"
  to = google_compute_instance.zedis
}

import {
  id = "projects/zedisv1/locations/global/workloadIdentityPools/github"
  to = google_iam_workload_identity_pool.github
}

import {
  id = "projects/zedisv1/locations/global/workloadIdentityPools/github/providers/github"
  to = google_iam_workload_identity_pool_provider.github
}

import {
  id = "projects/zedisv1/serviceAccounts/zedis-ci@zedisv1.iam.gserviceaccount.com"
  to = google_service_account.zedis_ci
}

import {
  id = "projects/zedisv1/serviceAccounts/zedis-vm@zedisv1.iam.gserviceaccount.com"
  to = google_service_account.zedis_vm
}

import {
  id = "projects/zedisv1/serviceAccounts/zedis-ci@zedisv1.iam.gserviceaccount.com roles/iam.workloadIdentityUser principalSet://iam.googleapis.com/projects/373840764878/locations/global/workloadIdentityPools/github/attribute.repository/BhagyeshKothalkar/zedis"
  to = google_service_account_iam_member.github_ci
}
