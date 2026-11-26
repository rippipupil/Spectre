import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
  id("com.android.application")
  id("org.jetbrains.kotlin.android")
}

// La CI pone la versión de la release (0.1.<número de la ejecución>), la
// misma que la de Spectre para el PC. El número de versión de Android tiene
// que crecer en cada release para que se instale encima de la anterior.
fun env(name: String): String? = System.getenv(name)?.takeIf { it.isNotEmpty() }
val holderVersionCode = (env("HOLDER_VERSION_CODE") ?: "1").toInt()
val holderVersionName = env("HOLDER_VERSION_NAME") ?: "0.1.0"

android {
  namespace = "com.spectre.holder"
  compileSdk = 35

  defaultConfig {
    applicationId = "com.spectre.holder"
    minSdk = 29
    targetSdk = 35
    versionCode = holderVersionCode
    versionName = holderVersionName
  }

  signingConfigs {
    create("release") {
      // Una clave fija: Android solo instala una versión nueva encima de la
      // anterior si las dos van firmadas con la misma. Con los secretos
      // HOLDER_KEYSTORE_* de GitHub se usa esa; si no, la del repositorio
      // (signing/README.md explica por qué y cómo cambiarla).
      val store = env("HOLDER_KEYSTORE_FILE")
      storeFile = if (store == null) rootProject.file("signing/spectre-holder.jks") else file(store)
      storePassword = if (store == null) "spectre-holder" else env("HOLDER_KEYSTORE_PASSWORD")
      keyAlias = if (store == null) "spectre-holder" else env("HOLDER_KEY_ALIAS")
      keyPassword = if (store == null) "spectre-holder" else env("HOLDER_KEY_PASSWORD")
    }
  }

  buildTypes {
    release {
      isMinifyEnabled = true
      isShrinkResources = true
      proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
      signingConfig = signingConfigs.getByName("release")
    }
    debug {
      applicationIdSuffix = ".debug"
    }
  }

  compileOptions {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
  }

  buildFeatures {
    buildConfig = false
  }

  packaging {
    resources.excludes += setOf("META-INF/*.version", "META-INF/*.kotlin_module", "kotlin/**", "DebugProbesKt.bin")
  }

  lint {
    // assembleRelease pasa la revisión de errores graves (lintVital); los
    // textos van en el código, en español, sin traducciones.
    disable += setOf("MissingTranslation", "SetTextI18n", "HardcodedText", "GradleDependency", "OldTargetApi")
  }
}

kotlin {
  compilerOptions {
    jvmTarget.set(JvmTarget.JVM_17)
  }
}

dependencies {
  // Solo para leer el QR del PC (Apache 2.0). Nada de AndroidX ni servicios de Google.
  implementation("com.google.zxing:core:3.5.3")
}
