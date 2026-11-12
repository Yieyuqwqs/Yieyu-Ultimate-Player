plugins {
    id("com.android.library") version "8.5.2"
    kotlin("android") version "2.0.20"
}

android {
    namespace = "com.yieyu.ultimate.service"
    compileSdk = 34
    defaultConfig {
        minSdk = 26                      // AAudio（模块 15）的硬门槛
        consumerProguardFiles("consumer-rules.pro")
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    buildTypes {
        release { isMinifyEnabled = true; proguardFiles("proguard-rules.pro") }
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.media:media:1.7.0")
}

// 铁律 7：Kotlin 总量封顶 400 行。超了就说明有业务漏进系统层。
val kotlinLineBudget = 400
tasks.register("checkKotlinBudget") {
    doLast {
        var lines = 0
        fileTree("src") { include("**/*.kt") }.forEach { lines += it.readLines().size }
        logger.lifecycle("Kotlin 行数: $lines / $kotlinLineBudget")
        if (lines > kotlinLineBudget) throw GradleException("Kotlin 超预算：$lines > $kotlinLineBudget")
    }
}
