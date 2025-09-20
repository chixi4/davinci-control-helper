using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace DualSensManager
{
    internal sealed class MainForm : Form
    {
        private readonly SettingsService settingsService = new SettingsService();
        private readonly DeviceSelectionState selectionState = new DeviceSelectionState();

        private List<MultiHandleDevice> devices = new List<MultiHandleDevice>();
        private Dictionary<IntPtr, MultiHandleDevice> handleMap = new Dictionary<IntPtr, MultiHandleDevice>();
        private double rightMultiplier = 1.0;
        private RawInputSource rawInputSource;
        private readonly LowLevelMouseBlocker mouseBlocker;
        private volatile bool blockRightInput = false;
        private bool mouseBlockerStarted = false;
        private readonly object autoPressGate = new object();
        private bool autoPressPressed = false;
        private HashSet<long> leftHandleSet = new HashSet<long>();
        private HashSet<long> rightHandleSet = new HashSet<long>();

        private readonly Label baseMultiplierValueLabel;
        private readonly Label rightDeviceLabel;
        private readonly Label leftDeviceLabel;
        private readonly Label statusLabel;
        private readonly NumericUpDown leftMultiplierInput;
        private readonly Button applyButton;
        private readonly CheckBox autoPressToggle;
        private readonly Button confirmRightButton;
        private readonly Button confirmLeftButton;

        // 最近一次成功刷新设备的时间，用于简单节流
        private DateTime lastReloadAt = DateTime.MinValue;

        // 最近一次接收到的 RAWINPUT 所属鼠标设备句柄（用于确认按钮来源判断）
        private IntPtr lastInputDeviceHandle = IntPtr.Zero;
        // 在确认按钮 MouseDown 捕获到的设备句柄
        private IntPtr confirmClickDeviceHandle = IntPtr.Zero;

        public MainForm()
        {
            mouseBlocker = new LowLevelMouseBlocker(() => blockRightInput);
            Text = "双鼠标灵敏度管理";
            StartPosition = FormStartPosition.CenterScreen;
            MinimumSize = new Size(600, 320);

            var layout = new TableLayoutPanel
            {
                Dock = DockStyle.Fill,
                ColumnCount = 4,
                RowCount = 6,
                Padding = new Padding(12),
                AutoSize = true
            };

            layout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            layout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            layout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));

            layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));

            var baseLabel = new Label
            {
                Text = "右手 sens multiplier:",
                AutoSize = true,
                Anchor = AnchorStyles.Left
            };
            baseMultiplierValueLabel = new Label
            {
                Text = "-",
                AutoSize = true,
                Anchor = AnchorStyles.Left,
                Font = new Font(Font, FontStyle.Bold)
            };

            layout.Controls.Add(baseLabel, 0, 0);
            layout.Controls.Add(baseMultiplierValueLabel, 1, 0);
            layout.SetColumnSpan(baseMultiplierValueLabel, 2);

            var rightLabel = new Label
            {
                Text = "右手鼠标:",
                AutoSize = true,
                Anchor = AnchorStyles.Left
            };
            rightDeviceLabel = CreateDeviceLabel();
            confirmRightButton = new Button
            {
                Text = "确认",
                AutoSize = true,
                Enabled = false
            };
            confirmRightButton.MouseDown += ConfirmButton_MouseDown;
            confirmRightButton.Click += (_, __) => ConfirmSelection(DetectionTarget.Right);
            var detectRightButton = new Button
            {
                Text = "检测右手",
                AutoSize = true
            };
            detectRightButton.Click += (_, __) => BeginDetection(DetectionTarget.Right);

            layout.Controls.Add(rightLabel, 0, 1);
            layout.Controls.Add(rightDeviceLabel, 1, 1);
            layout.Controls.Add(confirmRightButton, 2, 1);
            layout.Controls.Add(detectRightButton, 3, 1);

            var leftLabel = new Label
            {
                Text = "左手鼠标:",
                AutoSize = true,
                Anchor = AnchorStyles.Left
            };
            leftDeviceLabel = CreateDeviceLabel();
            confirmLeftButton = new Button
            {
                Text = "确认",
                AutoSize = true,
                Enabled = false
            };
            confirmLeftButton.MouseDown += ConfirmButton_MouseDown;
            confirmLeftButton.Click += (_, __) => ConfirmSelection(DetectionTarget.Left);
            var detectLeftButton = new Button
            {
                Text = "检测左手",
                AutoSize = true
            };
            detectLeftButton.Click += (_, __) => BeginDetection(DetectionTarget.Left);

            layout.Controls.Add(leftLabel, 0, 2);
            layout.Controls.Add(leftDeviceLabel, 1, 2);
            layout.Controls.Add(confirmLeftButton, 2, 2);
            layout.Controls.Add(detectLeftButton, 3, 2);

            var multiplierLabel = new Label
            {
                Text = "左手 sens multiplier:",
                AutoSize = true,
                Anchor = AnchorStyles.Left
            };
            leftMultiplierInput = new NumericUpDown
            {
                DecimalPlaces = 3,
                Minimum = 0.010M,
                Maximum = 5.000M,
                Increment = 0.050M,
                Value = 0.500M,
                Dock = DockStyle.Left,
                Width = 120
            };

            layout.Controls.Add(multiplierLabel, 0, 3);
            layout.Controls.Add(leftMultiplierInput, 1, 3);

            statusLabel = new Label
            {
                Text = "点击“检测”后，请用对应鼠标点击“确认”。",
                AutoSize = false,
                Dock = DockStyle.Fill,
                TextAlign = ContentAlignment.MiddleLeft
            };
            layout.Controls.Add(statusLabel, 0, 4);
            layout.SetColumnSpan(statusLabel, 4);

            var buttonsPanel = new FlowLayoutPanel
            {
                Dock = DockStyle.Fill,
                FlowDirection = FlowDirection.RightToLeft,
                AutoSize = true
            };

            applyButton = new Button
            {
                Text = "应用到驱动",
                AutoSize = true,
                Enabled = false
            };
            applyButton.Click += (_, __) => ApplySettings();

            autoPressToggle = new CheckBox
            {
                Text = "移动即按压",
                AutoSize = true
            };
            autoPressToggle.CheckedChanged += (_, __) => ToggleAutoPress();

            buttonsPanel.Controls.Add(applyButton);
            buttonsPanel.Controls.Add(autoPressToggle);
            layout.Controls.Add(buttonsPanel, 0, 5);
            layout.SetColumnSpan(buttonsPanel, 4);

            Controls.Add(layout);

            Load += OnLoad;
            Activated += OnActivated;
            FormClosed += OnFormClosed;
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            try
            {
                // 注册 RAW INPUT 到当前窗口，用于捕获“确认”按钮点击来源的物理鼠标设备
                RawInputInterop.RegisterForMouseMessages(this.Handle);
            }
            catch { /* ignore */ }
        }

        protected override void WndProc(ref Message m)
        {
            if (m.Msg == RawInputInterop.WM_INPUT)
            {
                // 记录最近一次产生输入的鼠标设备句柄
                IntPtr dev;
                int dx, dy;
                if (RawInputInterop.TryReadMouseInput(m.LParam, out dev, out dx, out dy))
                {
                    lastInputDeviceHandle = dev;
                }
            }
            else if (m.Msg == RawInputInterop.WM_INPUT_DEVICE_CHANGE)
            {
                // 设备热插拔时刷新列表
                BeginInvoke(new Action(() => ReloadDevices(false)));
            }
            base.WndProc(ref m);
        }

        private void OnLoad(object sender, EventArgs e)
        {
            try
            {
                ReloadDevices(false);
                var config = settingsService.LoadActiveConfig();
                rightMultiplier = settingsService.GetMultiplier(config.profiles[0]);
                baseMultiplierValueLabel.Text = FormatMultiplier(rightMultiplier);
                InitializeLeftMultiplierDefault();

                // 启动后台 RAW INPUT 事件源
                rawInputSource = new RawInputSource();
                rawInputSource.DeviceMoved += OnDeviceMoved;
                rawInputSource.DeviceListChanged += () => BeginInvoke(new Action(() => ReloadDevices(false)));
                rawInputSource.Start();
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "初始化失败: " + ex.Message, "错误", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        private void OnActivated(object sender, EventArgs e)
        {
            // 窗口激活时自动刷新一次（与上次刷新间隔 > 1 秒）
            if ((DateTime.UtcNow - lastReloadAt).TotalSeconds > 1)
            {
                ReloadDevices(false);
            }
        }

        private void InitializeLeftMultiplierDefault()
        {
            var suggested = Math.Max(0.010, rightMultiplier * 0.5);
            if ((double)leftMultiplierInput.Maximum < suggested)
            {
                suggested = (double)leftMultiplierInput.Maximum;
            }

            leftMultiplierInput.Value = Convert.ToDecimal(Math.Round(suggested, 3));
        }

        private void BeginDetection(DetectionTarget target)
        {
            // 进入检测前先确保设备列表是最新的
            ReloadDevices(false);
            selectionState.BeginDetection(target);
            selectionState.ClearTarget(target);
            UpdateDeviceLabels();
            UpdateApplyButtonState();
            if (autoPressToggle.Checked)
            {
                UpdateHandleSets();
            }

            // 仅进入等待确认态：不再因移动直接注册
            confirmClickDeviceHandle = IntPtr.Zero;
            confirmRightButton.Enabled = target == DetectionTarget.Right;
            confirmLeftButton.Enabled = target == DetectionTarget.Left;

            if (target == DetectionTarget.Left)
            {
                statusLabel.Text = "检测左手：请用左手鼠标点击“确认”。（候选 HID：等待输入）";
            }
            else if (target == DetectionTarget.Right)
            {
                statusLabel.Text = "检测右手：请用右手鼠标点击“确认”。（候选 HID：等待输入）";
            }
            else
            {
                statusLabel.Text = string.Empty;
            }
        }

        private void OnDeviceMoved(IntPtr deviceHandle, int deltaX, int deltaY)
        {
            // 无论是否在检测态，都记录最近活动的物理鼠标句柄
            lastInputDeviceHandle = deviceHandle;

            // 检测模式：用于锁定左右鼠标
            if (selectionState.CurrentTarget != DetectionTarget.None)
            {
                // 新逻辑：进入检测后仅等待“确认”按钮，不因移动直接注册
                // 但实时显示“候选 HID”以便用户确认
                BeginInvoke(new Action(() =>
                {
                    MultiHandleDevice device;
                    if (!handleMap.TryGetValue(deviceHandle, out device))
                    {
                        ReloadDevices(false);
                        handleMap.TryGetValue(deviceHandle, out device);
                    }

                    if (device != null)
                    {
                        var name = string.IsNullOrWhiteSpace(device.name) ? device.id : device.name;
                        if (selectionState.CurrentTarget == DetectionTarget.Left)
                        {
                            statusLabel.Text = "检测左手：请用左手鼠标点击“确认”。（候选 HID：" + name + " (" + device.id + ")）";
                        }
                        else if (selectionState.CurrentTarget == DetectionTarget.Right)
                        {
                            statusLabel.Text = "检测右手：请用右手鼠标点击“确认”。（候选 HID：" + name + " (" + device.id + ")）";
                        }
                    }
                }));
                return;
            }

            // 自动按压模式：后台线程立即注入
            if (autoPressToggle != null && autoPressToggle.Checked && selectionState.HasBothDevices)
            {
                var key = deviceHandle.ToInt64();
                var leftHit = false;
                lock (autoPressGate)
                {
                    var leftMatches = leftHandleSet.Count > 0 && leftHandleSet.Contains(key);
                    var rightMatches = rightHandleSet.Count > 0 && rightHandleSet.Contains(key);

                    if (leftMatches)
                    {
                        if (!autoPressPressed)
                        {
                            SendLeftDown();
                            autoPressPressed = true;
                        }
                        blockRightInput = true;
                        leftHit = true;
                    }
                    else if (rightMatches && autoPressPressed)
                    {
                        SendLeftUp();
                        autoPressPressed = false;
                        blockRightInput = false;
                    }
                }

                if (leftHit && blockRightInput && mouseBlockerStarted && (deltaX != 0 || deltaY != 0))
                {
                    SendRelativeMove(deltaX, deltaY);
                }
            }
        }

        private void OnFormClosed(object sender, FormClosedEventArgs e)
        {
            try
            {
                lock (autoPressGate)
                {
                    if (autoPressPressed)
                    {
                        SendLeftUp();
                        autoPressPressed = false;
                    }
                    blockRightInput = false;
                }
            }
            catch { /* ignore */ }
            try { mouseBlocker.Dispose(); } catch { /* ignore */ }
            mouseBlockerStarted = false;
            try { if (rawInputSource != null) rawInputSource.Stop(); } catch { /* ignore */ }
        }

        private void ReloadDevices(bool notify)
        {
            try
            {
                devices = settingsService.EnumerateDevices();
                handleMap = settingsService.BuildHandleLookup(devices);
                lastReloadAt = DateTime.UtcNow;
                if (notify)
                {
                    statusLabel.Text = "设备列表已刷新。";
                }
            }
            catch (Exception ex)
            {
                statusLabel.Text = "刷新设备失败: " + ex.Message;
            }

            UpdateDeviceLabels();
            UpdateApplyButtonState();
            if (autoPressToggle.Checked)
            {
                UpdateHandleSets();
            }
        }

        private void UpdateDeviceLabels()
        {
            rightDeviceLabel.Text = FormatDevice(selectionState.RightDeviceName, selectionState.RightDeviceId);
            leftDeviceLabel.Text = FormatDevice(selectionState.LeftDeviceName, selectionState.LeftDeviceId);
        }

        private void UpdateApplyButtonState()
        {
            applyButton.Enabled = selectionState.HasBothDevices;
        }

        private void ApplySettings()
        {
            try
            {
                var leftMultiplier = (double)leftMultiplierInput.Value;
                if (leftMultiplier >= rightMultiplier)
                {
                    var confirm = MessageBox.Show(
                        this,
                        "左手 multiplier 不低于右手，确定继续吗？",
                        "确认",
                        MessageBoxButtons.YesNo,
                        MessageBoxIcon.Warning);
                    if (confirm != DialogResult.Yes)
                    {
                        return;
                    }
                }

                var result = settingsService.Apply(
                    selectionState.RightDeviceId,
                    selectionState.RightDeviceName,
                    selectionState.LeftDeviceId,
                    selectionState.LeftDeviceName,
                    leftMultiplier);

                statusLabel.Text = "已写入驱动，左手 multiplier = " + FormatMultiplier(result.LeftMultiplier) + "。";
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "写入失败: " + ex.Message, "错误", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        private static Label CreateDeviceLabel()
        {
            return new Label
            {
                Text = "未设定",
                AutoSize = false,
                Dock = DockStyle.Fill,
                TextAlign = ContentAlignment.MiddleLeft
            };
        }

        private static string FormatDevice(string name, string id)
        {
            if (string.IsNullOrWhiteSpace(id))
            {
                return "未设定";
            }

            if (string.IsNullOrWhiteSpace(name) || string.Equals(name, id, StringComparison.OrdinalIgnoreCase))
            {
                return id;
            }

            return name + " (HID: " + id + ")";
        }

        private static string FormatMultiplier(double value)
        {
            return value.ToString("0.###", CultureInfo.InvariantCulture);
        }

        private bool StartMouseBlockerIfNeeded()
        {
            if (mouseBlockerStarted)
            {
                return true;
            }

            if (mouseBlocker.Start())
            {
                mouseBlockerStarted = true;
                return true;
            }

            return false;
        }

        private void ToggleAutoPress()
        {
            if (autoPressToggle.Checked)
            {
                if (!selectionState.HasBothDevices)
                {
                    autoPressToggle.Checked = false;
                    MessageBox.Show(this, "请先完成左右手鼠标检测", "提示", MessageBoxButtons.OK, MessageBoxIcon.Information);
                    return;
                }

                UpdateHandleSets();
                if (StartMouseBlockerIfNeeded())
                {
                    statusLabel.Text = "已开启移动即按压。";
                }
                else
                {
                    statusLabel.Text = "已开启移动即按压（右手屏蔽不可用）。";
                }
            }
            else
            {
                lock (autoPressGate)
                {
                    if (autoPressPressed)
                    {
                        SendLeftUp();
                        autoPressPressed = false;
                    }
                    blockRightInput = false;
                }

                mouseBlocker.Stop();
                mouseBlockerStarted = false;
                statusLabel.Text = "已关闭移动即按压。";
            }
        }

        private void UpdateHandleSets()
        {
            var left = FindDeviceById(selectionState.LeftDeviceId);
            var right = FindDeviceById(selectionState.RightDeviceId);
            lock (autoPressGate)
            {
                leftHandleSet = new HashSet<long>(left != null && left.handles != null ? ToKeys(left.handles) : new List<long>());
                rightHandleSet = new HashSet<long>(right != null && right.handles != null ? ToKeys(right.handles) : new List<long>());
            }
        }

        private static List<long> ToKeys(IEnumerable<IntPtr> ptrs)
        {
            var list = new List<long>();
            foreach (var p in ptrs) list.Add(p.ToInt64());
            return list;
        }

        private MultiHandleDevice FindDeviceById(string id)
        {
            foreach (var d in devices)
            {
                if (string.Equals(d.id, id, StringComparison.OrdinalIgnoreCase))
                {
                    return d;
                }
            }
            return null;
        }

        private void ConfirmButton_MouseDown(object sender, MouseEventArgs e)
        {
            // 在 MouseDown 阶段捕获触发点击的物理鼠标（通过最近一次 WM_INPUT）
            confirmClickDeviceHandle = lastInputDeviceHandle;
        }

        private void ConfirmSelection(DetectionTarget side)
        {
            if (selectionState.CurrentTarget != side)
            {
                statusLabel.Text = (side == DetectionTarget.Left) ? "请先点击“检测左手”。" : "请先点击“检测右手”。";
                return;
            }

            if (confirmClickDeviceHandle == IntPtr.Zero)
            {
                ReloadDevices(true);
                statusLabel.Text = "未能识别发起确认的鼠标，已自动刷新设备列表，请再试一次。";
                return;
            }

            MultiHandleDevice device;
            if (!handleMap.TryGetValue(confirmClickDeviceHandle, out device))
            {
                ReloadDevices(true);
                handleMap.TryGetValue(confirmClickDeviceHandle, out device);
            }

            if (device == null)
            {
                statusLabel.Text = "未能识别该鼠标，请再试一次。";
                return;
            }

            var name = string.IsNullOrWhiteSpace(device.name) ? device.id : device.name;
            if (side == DetectionTarget.Left && !string.IsNullOrEmpty(selectionState.RightDeviceId) && string.Equals(selectionState.RightDeviceId, device.id, StringComparison.OrdinalIgnoreCase))
            {
                selectionState.ClearTarget(DetectionTarget.Right);
                UpdateDeviceLabels();
                UpdateApplyButtonState();
                if (autoPressToggle.Checked)
                {
                    UpdateHandleSets();
                }
            }
            else if (side == DetectionTarget.Right && !string.IsNullOrEmpty(selectionState.LeftDeviceId) && string.Equals(selectionState.LeftDeviceId, device.id, StringComparison.OrdinalIgnoreCase))
            {
                selectionState.ClearTarget(DetectionTarget.Left);
                UpdateDeviceLabels();
                UpdateApplyButtonState();
                if (autoPressToggle.Checked)
                {
                    UpdateHandleSets();
                }
            }
            if (selectionState.ApplyDetectionResult(device.id, name))
            {
                statusLabel.Text = (side == DetectionTarget.Left) ? "已锁定左手鼠标。" : "已锁定右手鼠标。";
                UpdateDeviceLabels();
                UpdateApplyButtonState();
                if (autoPressToggle.Checked)
                {
                    UpdateHandleSets();
                }
            }

            // 退出等待态
            confirmRightButton.Enabled = false;
            confirmLeftButton.Enabled = false;
            confirmClickDeviceHandle = IntPtr.Zero;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct INPUT
        {
            public uint type;
            public MOUSEINPUT mi;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct MOUSEINPUT
        {
            public int dx;
            public int dy;
            public uint mouseData;
            public uint dwFlags;
            public uint time;
            public IntPtr dwExtraInfo;
        }

        private const uint INPUT_MOUSE = 0;
        private const uint MOUSEEVENTF_MOVE = 0x0001;
        private const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
        private const uint MOUSEEVENTF_LEFTUP = 0x0004;

        [DllImport("user32.dll", SetLastError = true)]
        private static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);

        private static void SendRelativeMove(int dx, int dy)
        {
            if (dx == 0 && dy == 0)
            {
                return;
            }

            var input = new INPUT
            {
                type = INPUT_MOUSE,
                mi = new MOUSEINPUT
                {
                    dx = dx,
                    dy = dy,
                    dwFlags = MOUSEEVENTF_MOVE
                }
            };

            SendInput(1, new[] { input }, Marshal.SizeOf(typeof(INPUT)));
        }

        private static void SendLeftDown()
        {
            var input = new INPUT { type = INPUT_MOUSE, mi = new MOUSEINPUT { dwFlags = MOUSEEVENTF_LEFTDOWN } };
            SendInput(1, new[] { input }, Marshal.SizeOf(typeof(INPUT)));
        }

        private static void SendLeftUp()
        {
            var input = new INPUT { type = INPUT_MOUSE, mi = new MOUSEINPUT { dwFlags = MOUSEEVENTF_LEFTUP } };
            SendInput(1, new[] { input }, Marshal.SizeOf(typeof(INPUT)));
        }
    }
}
