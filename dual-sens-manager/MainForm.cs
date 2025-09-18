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

        public MainForm()
        {
            Text = "双鼠标灵敏度管理";
            StartPosition = FormStartPosition.CenterScreen;
            MinimumSize = new Size(600, 320);

            var layout = new TableLayoutPanel
            {
                Dock = DockStyle.Fill,
                ColumnCount = 3,
                RowCount = 6,
                Padding = new Padding(12),
                AutoSize = true
            };

            layout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
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
            var detectRightButton = new Button
            {
                Text = "检测右手",
                AutoSize = true
            };
            detectRightButton.Click += (_, __) => BeginDetection(DetectionTarget.Right);

            layout.Controls.Add(rightLabel, 0, 1);
            layout.Controls.Add(rightDeviceLabel, 1, 1);
            layout.Controls.Add(detectRightButton, 2, 1);

            var leftLabel = new Label
            {
                Text = "左手鼠标:",
                AutoSize = true,
                Anchor = AnchorStyles.Left
            };
            leftDeviceLabel = CreateDeviceLabel();
            var detectLeftButton = new Button
            {
                Text = "检测左手",
                AutoSize = true
            };
            detectLeftButton.Click += (_, __) => BeginDetection(DetectionTarget.Left);

            layout.Controls.Add(leftLabel, 0, 2);
            layout.Controls.Add(leftDeviceLabel, 1, 2);
            layout.Controls.Add(detectLeftButton, 2, 2);

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
                Text = "点击“检测”按钮并晃动对应的鼠标。",
                AutoSize = false,
                Dock = DockStyle.Fill,
                TextAlign = ContentAlignment.MiddleLeft
            };
            layout.Controls.Add(statusLabel, 0, 4);
            layout.SetColumnSpan(statusLabel, 3);

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

            var refreshButton = new Button
            {
                Text = "刷新设备",
                AutoSize = true
            };
            refreshButton.Click += (_, __) => ReloadDevices(true);

            autoPressToggle = new CheckBox
            {
                Text = "移动即按压",
                AutoSize = true
            };
            autoPressToggle.CheckedChanged += (_, __) => ToggleAutoPress();

            buttonsPanel.Controls.Add(applyButton);
            buttonsPanel.Controls.Add(refreshButton);
            buttonsPanel.Controls.Add(autoPressToggle);
            layout.Controls.Add(buttonsPanel, 0, 5);
            layout.SetColumnSpan(buttonsPanel, 3);

            Controls.Add(layout);

            Load += OnLoad;
            FormClosed += OnFormClosed;
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
        }

        protected override void WndProc(ref Message m)
        {
            base.WndProc(ref m);
        }

        private void OnLoad(object sender, EventArgs e)
        {
            try
            {
                var config = settingsService.LoadActiveConfig();
                rightMultiplier = settingsService.GetMultiplier(config.profiles[0]);
                baseMultiplierValueLabel.Text = FormatMultiplier(rightMultiplier);
                InitializeLeftMultiplierDefault();
                ReloadDevices(false);

                // 启动后台 RAW INPUT 事件源
                rawInputSource = new RawInputSource();
                rawInputSource.DeviceMoved += OnDeviceMoved;
                rawInputSource.DeviceListChanged += () => BeginInvoke(new Action(() => ReloadDevices(false)));
                rawInputSource.Start();
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, $"初始化失败: {ex.Message}", "错误", MessageBoxButtons.OK, MessageBoxIcon.Error);
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
            selectionState.BeginDetection(target);
            statusLabel.Text = target switch
            {
                DetectionTarget.Left => "请晃动左手鼠标...",
                DetectionTarget.Right => "请晃动右手鼠标...",
                _ => ""
            };
        }

        private void OnDeviceMoved(IntPtr deviceHandle, int deltaX, int deltaY)
        {
            // 检测模式：用于锁定左右鼠标
            if (selectionState.CurrentTarget != DetectionTarget.None)
            {
                if (deltaX == 0 && deltaY == 0) return;

                BeginInvoke(new Action(() =>
                {
                    if (!handleMap.TryGetValue(deviceHandle, out var device))
                    {
                        ReloadDevices(false);
                        handleMap.TryGetValue(deviceHandle, out device);
                    }

                    if (device == null)
                    {
                        statusLabel.Text = "未能识别该鼠标，请重试或刷新设备。";
                        return;
                    }

                    var name = string.IsNullOrWhiteSpace(device.name) ? device.id : device.name;
                    var target = selectionState.CurrentTarget;
                    if (selectionState.ApplyDetectionResult(device.id, name))
                    {
                        statusLabel.Text = target == DetectionTarget.Left ? "已锁定左手鼠标。" : "已锁定右手鼠标。";
                        UpdateDeviceLabels();
                        UpdateApplyButtonState();
                        if (autoPressToggle.Checked)
                        {
                            UpdateHandleSets();
                        }
                    }
                }));
                return;
            }

            // 自动按压模式：后台线程立即注入
            if (autoPressToggle != null && autoPressToggle.Checked && selectionState.HasBothDevices)
            {
                var key = deviceHandle.ToInt64();
                lock (autoPressGate)
                {
                    if ((leftHandleSet.Count > 0 && leftHandleSet.Contains(key)) && !autoPressPressed)
                    {
                        SendLeftDown();
                        autoPressPressed = true;
                    }
                    else if ((rightHandleSet.Count > 0 && rightHandleSet.Contains(key)) && autoPressPressed)
                    {
                        SendLeftUp();
                        autoPressPressed = false;
                    }
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
                }
            }
            catch { /* ignore */ }
            try { rawInputSource?.Stop(); } catch { /* ignore */ }
        }

        private void ReloadDevices(bool notify)
        {
            try
            {
                devices = settingsService.EnumerateDevices();
                handleMap = settingsService.BuildHandleLookup(devices);
                if (notify)
                {
                    statusLabel.Text = "设备列表已刷新。";
                }
            }
            catch (Exception ex)
            {
                statusLabel.Text = $"刷新设备失败: {ex.Message}";
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

                statusLabel.Text = $"已写入驱动，左手 multiplier = {FormatMultiplier(result.LeftMultiplier)}。";
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, $"写入失败: {ex.Message}", "错误", MessageBoxButtons.OK, MessageBoxIcon.Error);
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

            return $"{name} (HID: {id})";
        }

        private static string FormatMultiplier(double value)
        {
            return value.ToString("0.###", CultureInfo.InvariantCulture);
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
                statusLabel.Text = "已开启移动即按压。";
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
                }
                statusLabel.Text = "已关闭移动即按压。";
            }
        }

        private void UpdateHandleSets()
        {
            var left = FindDeviceById(selectionState.LeftDeviceId);
            var right = FindDeviceById(selectionState.RightDeviceId);
            lock (autoPressGate)
            {
                leftHandleSet = new HashSet<long>(left?.handles != null ? ToKeys(left.handles) : new List<long>());
                rightHandleSet = new HashSet<long>(right?.handles != null ? ToKeys(right.handles) : new List<long>());
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
        private const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
        private const uint MOUSEEVENTF_LEFTUP = 0x0004;

        [DllImport("user32.dll", SetLastError = true)]
        private static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);

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
