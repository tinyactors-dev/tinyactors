plugins {
    kotlin("jvm") version "2.4.21"
    kotlin("plugin.serialization") version "2.4.21"
    application
}

repositories { mavenCentral() }

dependencies {
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-cbor:1.13.0")
}

kotlin { jvmToolchain(21) }

application { mainClass.set("jsonbench.MainKt") }
