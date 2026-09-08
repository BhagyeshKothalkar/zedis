terraform {
  required_version = ">= 1.6.0"

  required_providers {
    google = {
      source  = "hashicorp/google"
      version = "~> 7.0"
    }
  }
}

provider "google" {
  project = "zedisv1"
  region  = "asia-south1"
  zone    = "asia-south1-b"
}
