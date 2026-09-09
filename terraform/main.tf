resource "google_compute_instance" "zedis" {
  name         = "zedis-vm"
  machine_type = "c3-standard-4"
  zone         = "asia-south1-b"

  metadata = {
    enable-oslogin = "TRUE"
    startup-script = file("${path.module}/startup.sh")
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

  # Prometheus, Grafana, and Node Exporter bind to VM loopback in Compose and
  # intentionally have no public firewall rules.
  target_tags = ["zedis"]
}

resource "google_service_account" "zedis_ci" {
  account_id   = "zedis-ci"
  display_name = "Zedis GitHub Actions CI"
}
