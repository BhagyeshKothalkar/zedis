resource "google_compute_instance" "zedis" {
  name         = "zedis-vm"
  machine_type = "c3-standard-4"
  zone         = "asia-south1-b"

  boot_disk {
    initialize_params {
      image = "ubuntu-2404-noble-amd64-v20260906"
      size  = 10
    }
  }

  network_interface {
    network = "default"

    access_config {
      # Ephemeral public IP
    }
  }

  tags = ["zedis"]

  service_account {
    email = google_service_account.zedis_vm.email

    scopes = [
      "https://www.googleapis.com/auth/cloud-platform"
    ]
  }
}
