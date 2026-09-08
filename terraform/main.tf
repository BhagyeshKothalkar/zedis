resource "google_compute_instance" "zedis" {
  name         = "zedis-vm"
  machine_type = "c3-standard-4"
  zone         = "asia-south1-b"

  metadata = {
    startup-script = templatefile("${path.module}/startup.sh", {
      zedis_image = var.zedis_image
    })
  }

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

resource "google_compute_firewall" "zedis" {
  name    = "allow-zedis"
  network = "default"

  allow {
    protocol = "tcp"
    ports    = ["16379"]
  }

  source_ranges = ["0.0.0.0/0"]

  target_tags = ["zedis"]
}

resource "google_service_account" "zedis_ci" {
  account_id   = "zedis-ci"
  display_name = "Zedis GitHub Actions CI"
}
