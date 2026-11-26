package com.spectre.holder

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder

// Servicio en primer plano solo mientras hay descargas: Android no congela la
// app con la pantalla apagada y la notificación enseña cómo va. Se para solo
// al vaciarse la cola.
class TransferService : Service() {
  private val update: () -> Unit = { refresh() }
  private var started = false

  override fun onBind(intent: Intent?): IBinder? = null

  override fun onCreate() {
    super.onCreate()
    Holder.init(this)
    val nm = getSystemService(NotificationManager::class.java) ?: return
    if (nm.getNotificationChannel(CHANNEL) == null) {
      nm.createNotificationChannel(
        NotificationChannel(CHANNEL, "Descargas", NotificationManager.IMPORTANCE_LOW).apply {
          description = "Lo que se está descargando de la nube a la galería"
          setShowBadge(false)
        },
      )
    }
  }

  override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
    val n = build()
    try {
      if (Build.VERSION.SDK_INT >= 29) {
        startForeground(ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
      } else {
        startForeground(ID, n)
      }
    } catch (e: Exception) {
      stopSelf()
      return START_NOT_STICKY
    }
    if (!started) {
      started = true
      Holder.listen(update)
    }
    refresh()
    return START_NOT_STICKY
  }

  override fun onDestroy() {
    Holder.unlisten(update)
    super.onDestroy()
  }

  // Android 15: un servicio de datos tiene un tiempo máximo al día.
  override fun onTimeout(startId: Int, fgsType: Int) {
    stopSelf()
  }

  private fun refresh() {
    if (!Holder.busy()) {
      stopForeground(STOP_FOREGROUND_REMOVE)
      stopSelf()
      return
    }
    getSystemService(NotificationManager::class.java)?.notify(ID, build())
  }

  private fun build(): Notification {
    val c = Holder.current
    val pending = Holder.pending()
    val title = if (c != null) "Descargando ${c.title}" else "Descargando"
    val pct = (Holder.progress * 100).toInt().coerceIn(0, 100)
    val text = buildString {
      append("$pct %")
      if (Holder.speed > 0) append(" · ").append(Fmt.bytes(Holder.speed.toLong())).append("/s")
      if (pending > 1) append(" · quedan ").append(pending - 1)
    }
    val open = PendingIntent.getActivity(
      this, 0, Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
      PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
    )
    return Notification.Builder(this, CHANNEL)
      .setSmallIcon(android.R.drawable.stat_sys_download)
      .setContentTitle(title)
      .setContentText(text)
      .setProgress(100, pct, c == null)
      .setOngoing(true)
      .setOnlyAlertOnce(true)
      .setContentIntent(open)
      .setCategory(Notification.CATEGORY_PROGRESS)
      .build()
  }

  companion object {
    private const val CHANNEL = "descargas"
    private const val ID = 1
  }
}
