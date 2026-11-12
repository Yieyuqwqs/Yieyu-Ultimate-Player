package com.yieyu.ultimate.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.media.session.MediaSession
import android.media.session.PlaybackState
import android.os.Build
import android.os.IBinder

/**
 * 【模块 22】前台服务保活 —— 全场唯一 Kotlin。
 *
 * 只干一件事：让系统知道"我在放音乐"，从而不被杀。
 * 业务判定（能不能播、播什么、卡密是否有效）一律不在这里：
 * 这个类总共只有 3 个入口：onCreate / onStartCommand / 通知栏按钮。
 *
 * 与 C++ 的接口只有 3 个符号（见 core/src/jsb/jni_service.cpp，唯一允许的 JNI 面）：
 *   nativePing() / nativeAction(id) / nativeSnapshot(json)
 */
class PlaybackService : Service() {

    private var session: MediaSession? = null

    override fun onCreate() {
        super.onCreate()
        createChannel()
        session = MediaSession(this, "YieyuUltimate").apply {
            setCallback(object : MediaSession.Callback() {
                override fun onPlay() { action(ACTION_PLAY) }
                override fun onPause() { action(ACTION_PAUSE) }
                override fun onSkipToNext() { action(ACTION_NEXT) }
                override fun onSkipToPrevious() { action(ACTION_PREV) }
            })
            isActive = true
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_PLAY -> action(ACTION_PLAY)
            ACTION_PAUSE -> action(ACTION_PAUSE)
            ACTION_NEXT -> action(ACTION_NEXT)
            ACTION_PREV -> action(ACTION_PREV)
            else -> Unit
        }
        // 启动即进前台：先给一个"占位通知"，真状态由 C++ 侧回灌（不在这儿猜）
        startForeground(NOTIF_ID, buildNotification(playing = nativePing() == 1))
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun action(id: Int) {
        // 只转发，不判定。真实状态变化由 core 通过 JSB 广播回 Flutter
        nativeAction(id)
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        nm.notify(NOTIF_ID, buildNotification(playing = id == ACTION_PLAY))
        session?.setPlaybackState(
            PlaybackState.Builder()
                .setActions(PlaybackState.ACTION_PLAY or PlaybackState.ACTION_PAUSE)
                .setState(
                    if (id == ACTION_PLAY) PlaybackState.STATE_PLAYING else PlaybackState.STATE_PAUSED,
                    PlaybackState.PLAYBACK_POSITION_UNKNOWN, 1.0f
                ).build()
        )
    }

    private fun createChannel() {
        val ch = NotificationChannel(CHANNEL_ID, "播放", NotificationManager.IMPORTANCE_LOW).apply {
            setShowBadge(false)
            description = "播放控制与进度"
        }
        (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager).createNotificationChannel(ch)
    }

    private fun buildNotification(playing: Boolean): Notification {
        val open = PendingIntent.getActivity(
            this, 0,
            packageManager.getLaunchIntentForPackage(packageName),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
        val b = Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("Yieyu Ultimate Player")
            .setContentText(if (playing) "正在播放" else "已暂停")
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setContentIntent(open)
            .setOngoing(playing)
            .setVisibility(Notification.VISIBILITY_PUBLIC)

        fun act(icon: Int, title: String, code: Int): Notification.Action {
            val pi = PendingIntent.getService(
                this, code,
                Intent(this, PlaybackService::class.java).setAction(
                    when (code) { ACTION_PLAY -> ACTION_PLAY; ACTION_PAUSE -> ACTION_PAUSE; ACTION_NEXT -> ACTION_NEXT; else -> ACTION_PREV }
                ),
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
            )
            return Notification.Action.Builder(android.graphics.drawable.Icon.createWithResource(this, icon), title, pi).build()
        }
        b.addAction(act(android.R.drawable.ic_media_previous, "上一首", ACTION_PREV))
        if (playing) b.addAction(act(android.R.drawable.ic_media_pause, "暂停", ACTION_PAUSE))
        else b.addAction(act(android.R.drawable.ic_media_play, "播放", ACTION_PLAY))
        b.addAction(act(android.R.drawable.ic_media_next, "下一首", ACTION_NEXT))
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) b.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE)
        return b.build()
    }

    // ── 唯一允许的 JNI 面（3 个符号，全部只读/只转发）──────────────
    private external fun nativePing(): Int
    private external fun nativeAction(id: Int)

    companion object {
        private const val CHANNEL_ID = "yieyu.playback"
        private const val NOTIF_ID = 0x5945
        const val ACTION_PLAY = 0
        const val ACTION_PAUSE = 1
        const val ACTION_NEXT = 2
        const val ACTION_PREV = 3

        init { System.loadLibrary("yieyu_core") }

        fun start(ctx: Context) {
            val i = Intent(ctx, PlaybackService::class.java)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) ctx.startForegroundService(i) else ctx.startService(i)
        }
    }
}
