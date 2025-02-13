/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/


#include "JoystickConfigController.h"
#include "JoystickManager.h"
#include "QGCApplication.h"
#include "MultiVehicleManager.h"
#include "Vehicle.h"
#include "QGCLoggingCategory.h"

QGC_LOGGING_CATEGORY(JoystickConfigControllerLog, "JoystickConfigControllerLog")

JoystickConfigController::JoystickConfigController(void)
{
    
    connect(JoystickManager::instance(), &JoystickManager::activeJoystickChanged, this, &JoystickConfigController::_activeJoystickChanged);
    _activeJoystickChanged(JoystickManager::instance()->activeJoystick());
    _setStickPositions();
    _resetInternalCalibrationValues();
    _currentStickPositions  << _sticksCentered.leftX  << _sticksCentered.leftY  << _sticksCentered.rightX  << _sticksCentered.rightY;
}

void JoystickConfigController::start(void)
{
    _stopCalibration();
}

void JoystickConfigController::setDeadbandValue(int axis, int value)
{
    _axisDeadbandChanged(axis,value);
    Joystick* joystick = JoystickManager::instance()->activeJoystick();
    Joystick::Calibration_t calibration = joystick->getCalibration(axis);
    calibration.deadband = value;
    joystick->setCalibration(axis,calibration);
}

JoystickConfigController::~JoystickConfigController()
{
    if(_activeJoystick) {
        _activeJoystick->setCalibrationMode(false);
    }
}

/// @brief Returns the state machine entry for the specified state.
const JoystickConfigController::stateMachineEntry* JoystickConfigController::_getStateMachineEntry(int step)
{
    static constexpr const char* msgBegin =               "Allow all sticks to center as shown in diagram.\nClick Next to continue";
    static constexpr const char* msgThrottleUp =          "Move the Throttle stick all the way up and hold it there...";
    static constexpr const char* msgThrottleDown =        "Move the Throttle stick all the way down and hold it there...";
    static constexpr const char* msgYawLeft =             "Move the Yaw stick all the way to the left and hold it there...";
    static constexpr const char* msgYawRight =            "Move the Yaw stick all the way to the right and hold it there...";
    static constexpr const char* msgRollLeft =            "Move the Roll stick all the way to the left and hold it there...";
    static constexpr const char* msgRollRight =           "Move the Roll stick all the way to the right and hold it there...";
    static constexpr const char* msgPitchDown =           "Move the Pitch stick all the way down and hold it there...";
    static constexpr const char* msgPitchUp =             "Move the Pitch stick all the way up and hold it there...";
    static constexpr const char* msgPitchCenter =         "Allow the Pitch stick to move back to center...";
    static constexpr const char* msgComplete =            "All settings have been captured.\nClick Next to enable the joystick.";

    static const stateMachineEntry rgStateMachine[] = {
        //Function
        { Joystick::maxFunction,            msgBegin,           _sticksCentered,        &JoystickConfigController::_inputCenterWaitBegin,   &JoystickConfigController::_saveAllTrims,        nullptr, 0 },
        { Joystick::throttleFunction,       msgThrottleUp,      _sticksThrottleUp,      &JoystickConfigController::_inputStickDetect,       nullptr,                                         nullptr, 0 },
        { Joystick::throttleFunction,       msgThrottleDown,    _sticksThrottleDown,    &JoystickConfigController::_inputStickMin,          nullptr,                                         nullptr, 0 },
        { Joystick::yawFunction,            msgYawRight,        _sticksYawRight,        &JoystickConfigController::_inputStickDetect,       nullptr,                                         nullptr, 1 },
        { Joystick::yawFunction,            msgYawLeft,         _sticksYawLeft,         &JoystickConfigController::_inputStickMin,          nullptr,                                         nullptr, 1 },
        { Joystick::rollFunction,           msgRollRight,       _sticksRollRight,       &JoystickConfigController::_inputStickDetect,       nullptr,                                         nullptr, 2 },
        { Joystick::rollFunction,           msgRollLeft,        _sticksRollLeft,        &JoystickConfigController::_inputStickMin,          nullptr,                                         nullptr, 2 },
        { Joystick::pitchFunction,          msgPitchUp,         _sticksPitchUp,         &JoystickConfigController::_inputStickDetect,       nullptr,                                         nullptr, 3 },
        { Joystick::pitchFunction,          msgPitchDown,       _sticksPitchDown,       &JoystickConfigController::_inputStickMin,          nullptr,                                         nullptr, 3 },
        { Joystick::pitchFunction,          msgPitchCenter,     _sticksCentered,        &JoystickConfigController::_inputCenterWait,        nullptr,                                         nullptr, 3 },
        { Joystick::maxFunction,            msgComplete,        _sticksCentered,        nullptr,                                            &JoystickConfigController::_writeCalibration,    nullptr, -1 },
    };

    Q_ASSERT(step >= 0 && step < static_cast<int>((sizeof(rgStateMachine) / sizeof(rgStateMachine[0]))));
    return &rgStateMachine[step];
}

void JoystickConfigController::_advanceState()
{
    _currentStep++;
    _setupCurrentState();
}

bool JoystickConfigController::nextEnabled()
{
    if(_currentStep >= 0) {
        const stateMachineEntry* state = _getStateMachineEntry(_currentStep);
        return state->nextFn != nullptr;
    }
    return false;
}

bool JoystickConfigController::skipEnabled()
{
    if(_currentStep >= 0) {
        const stateMachineEntry* state = _getStateMachineEntry(_currentStep);
        return state->skipFn != nullptr;
    }
    return false;
}

/// @brief Sets up the state machine according to the current step from _currentStep.
void JoystickConfigController::_setupCurrentState()
{
    const stateMachineEntry* state = _getStateMachineEntry(_currentStep);
    _setStatusText(state->instructions);
    _stickDetectAxis = _axisNoAxis;
    _stickDetectSettleStarted = false;
    _calSaveCurrentValues();
    _currentStickPositions.clear();
    _currentStickPositions << state->stickPositions.leftX << state->stickPositions.leftY << state->stickPositions.rightX << state->stickPositions.rightY;
    emit stickPositionsChanged();
    emit nextEnabledChanged();
    emit skipEnabledChanged();
}

void JoystickConfigController::_axisValueChanged(int axis, int value)
{
    if (_validAxis(axis)) {
        // We always update raw values
        _axisRawValue[axis] = value;
        emit axisValueChanged(axis, _axisRawValue[axis]);
        if (_currentStep == -1) {
            // Track the axis count by keeping track of how many axes we see
            if (axis + 1 > static_cast<int>(_axisCount)) {
                _axisCount = axis + 1;
            }
        }
        if (_currentStep != -1) {
            const stateMachineEntry* state = _getStateMachineEntry(_currentStep);
            Q_ASSERT(state);
            if (state->rcInputFn) {
                (this->*state->rcInputFn)(state->function, axis, value);
            }
        }
    }
}

void JoystickConfigController::nextButtonClicked()
{
    if (_currentStep == -1) {
        // Need to have enough channels
        if (_axisCount < _axisMinimum) {
            qgcApp()->showAppMessage(tr("Detected %1 joystick axes. To operate PX4, you need at least %2 axes.").arg(_axisCount).arg(_axisMinimum));
            return;
        }
        _startCalibration();
    } else {
        const stateMachineEntry* state = _getStateMachineEntry(_currentStep);
        Q_ASSERT(state);
        Q_ASSERT(state->nextFn);
        (this->*state->nextFn)();
    }
}

void JoystickConfigController::skipButtonClicked()
{
    Q_ASSERT(_currentStep != -1);
    const stateMachineEntry* state = _getStateMachineEntry(_currentStep);
    Q_ASSERT(state);
    Q_ASSERT(state->skipFn);
    (this->*state->skipFn)();
}

void JoystickConfigController::cancelButtonClicked(void)
{
    _stopCalibration();
}

bool JoystickConfigController::getDeadbandToggle() {
    return _activeJoystick->deadband();
}

void JoystickConfigController::setDeadbandToggle(bool deadband) {
    _activeJoystick->setDeadband(deadband);
    _signalAllAttitudeValueChanges();
    emit deadbandToggled(deadband);
}

void JoystickConfigController::_saveAllTrims()
{
    // We save all trims as the first step. At this point no axes are mapped but it should still
    // allow us to get good trims for the roll/pitch/yaw/throttle even though we don't know which
    // axis they are yet. As we continue through the process the other axes will get their
    // trims reset to correct values.
    for (int i = 0; i < _axisCount; i++) {
        qCDebug(JoystickConfigControllerLog) << "_saveAllTrims trim" << _axisRawValue[i];
        _rgAxisInfo[i].axisTrim = _axisRawValue[i];
    }
    _advanceState();
}

void JoystickConfigController::_axisDeadbandChanged(int axis, int value)
{
    value = abs(value)<_calValidMaxValue?abs(value):_calValidMaxValue;
    _rgAxisInfo[axis].deadband = value;
    emit axisDeadbandChanged(axis,value);
    qCDebug(JoystickConfigControllerLog) << "Axis:" << axis << "Deadband:" << _rgAxisInfo[axis].deadband;
}

/// @brief Waits for the sticks to be centered, enabling Next when done.
void JoystickConfigController::_inputCenterWaitBegin(Joystick::AxisFunction_t function, int axis, int value)
{
    Q_UNUSED(function);
    //sensing deadband
    if ((abs(value) * 1.1f > _rgAxisInfo[axis].deadband) && (_activeJoystick->deadband())) {   //add 10% on top of existing deadband
        _axisDeadbandChanged(axis, static_cast<int>(abs(value) * 1.1f));
    }
    // FIXME: Doesn't wait for center
}

bool JoystickConfigController::_stickSettleComplete(int axis, int value)
{
    if (!_validAxis(axis)) {
        qCWarning(JoystickConfigControllerLog) << "Invalid axis axis:_axisCount" << axis << _axisCount;
        return false;
    }

    // We are waiting for the stick to settle out to a max position
    
    if (abs(_stickDetectValue - value) > _calSettleDelta) {
        // Stick is moving too much to consider stopped
        qCDebug(JoystickConfigControllerLog) << "_stickSettleComplete still moving, axis:_stickDetectValue:value" << axis << _stickDetectValue << value;
        _stickDetectValue = value;
        _stickDetectSettleStarted = false;
    } else {
        // Stick is still positioned within the specified small range
        if (_stickDetectSettleStarted) {
            // We have already started waiting
            if (_stickDetectSettleElapsed.elapsed() > _stickDetectSettleMSecs) {
                // Stick has stayed positioned in one place long enough, detection is complete.
                qCDebug(JoystickConfigControllerLog) << "_stickSettleComplete detection complete, axis:_stickDetectValue:value" << axis << _stickDetectValue << value;
                return true;
            }
        } else {
            // Start waiting for the stick to stay settled for _stickDetectSettleWaitMSecs msecs
            qCDebug(JoystickConfigControllerLog) << "_stickSettleComplete starting settle timer, axis:_stickDetectValue:value" << axis << _stickDetectValue << value;
            _stickDetectSettleStarted = true;
            _stickDetectSettleElapsed.start();
        }
    }
    
    return false;
}

void JoystickConfigController::_inputStickDetect(Joystick::AxisFunction_t function, int axis, int value)
{
    qCDebug(JoystickConfigControllerLog) << "_inputStickDetect function:axis:value" << function << axis << value;
    
    if (!_validAxis(axis)) {
        qCWarning(JoystickConfigControllerLog) << "Invalid axis axis:_axisCount" << axis << _axisCount;
        return;
    }

    // If this axis is already used in a mapping we can't use it again
    if (_rgAxisInfo[axis].function != Joystick::maxFunction) {
        return;
    }
    
    if (_stickDetectAxis == _axisNoAxis) {
        // We have not detected enough movement on a axis yet
        if (abs(_axisValueSave[axis] - value) > _calMoveDelta) {
            // Stick has moved far enough to consider it as being selected for the function
            qCDebug(JoystickConfigControllerLog) << "_inputStickDetect starting settle wait, function:axis:value" << function << axis << value;
            // Setup up to detect stick being pegged to min or max value
            _stickDetectAxis = axis;
            _stickDetectInitialValue = value;
            _stickDetectValue = value;
        }
    } else if (axis == _stickDetectAxis) {
        if (_stickSettleComplete(axis, value)) {
            AxisInfo* info = &_rgAxisInfo[axis];
            // Stick detection is complete. Stick should be at max position.
            // Map the axis to the function
            _rgFunctionAxisMapping[function] = axis;
            info->function = function;
            // Axis should be at max value, if it is below initial set point the the axis is reversed.
            info->reversed = value < _axisValueSave[axis];
            if (info->reversed) {
                _rgAxisInfo[axis].axisMin = value;
            } else {
                _rgAxisInfo[axis].axisMax = value;
            }
            qCDebug(JoystickConfigControllerLog) << "_inputStickDetect saving values, function:axis:value:reversed:_axisValueSave" << function << axis << value << info->reversed << _axisValueSave[axis];
            _signalAllAttitudeValueChanges();
            _advanceState();
        }
    }
}

void JoystickConfigController::_inputStickMin(Joystick::AxisFunction_t function, int axis, int value)
{
    qCDebug(JoystickConfigControllerLog) << "_inputStickMin function:axis:value" << function << axis << value;
    if (!_validAxis(axis)) {
        qCWarning(JoystickConfigControllerLog) << "Invalid axis axis:_axisCount" << axis << _axisCount;
        return;
    }
    // We only care about the axis mapped to the function we are working on
    if (_rgFunctionAxisMapping[function] != axis) {
        return;
    }
    if (_stickDetectAxis == _axisNoAxis) {
        // Setup up to detect stick being pegged to extreme position
        if (_rgAxisInfo[axis].reversed) {
            if (value > _calCenterPoint + _calMoveDelta) {
                _stickDetectAxis = axis;
                _stickDetectInitialValue = value;
                _stickDetectValue = value;
                qCDebug(JoystickConfigControllerLog) << "_inputStickMin detected movement _stickDetectAxis:_stickDetectInitialValue" << _stickDetectAxis << _stickDetectInitialValue;
            }
        } else {
            if (value < _calCenterPoint - _calMoveDelta) {
                _stickDetectAxis = axis;
                _stickDetectInitialValue = value;
                _stickDetectValue = value;
                qCDebug(JoystickConfigControllerLog) << "_inputStickMin detected movement _stickDetectAxis:_stickDetectInitialValue" << _stickDetectAxis << _stickDetectInitialValue;
            }
        }
    } else {
        // We are waiting for the selected axis to settle out
        if (_stickSettleComplete(axis, value)) {
            AxisInfo* info = &_rgAxisInfo[axis];
            // Stick detection is complete. Stick should be at min position.
            if (info->reversed) {
                _rgAxisInfo[axis].axisMax = value;
            } else {
                _rgAxisInfo[axis].axisMin = value;
            }
            qCDebug(JoystickConfigControllerLog) << "_inputStickMin saving values, function:axis:value:reversed" << function << axis << value << info->reversed;
            _advanceState();
        }
    }
}

void JoystickConfigController::_inputCenterWait(Joystick::AxisFunction_t function, int axis, int value)
{
    qCDebug(JoystickConfigControllerLog) << "_inputCenterWait function:axis:value" << function << axis << value;
    if (!_validAxis(axis)) {
        qCWarning(JoystickConfigControllerLog) << "Invalid axis axis:_axisCount" << axis << _axisCount;
        return;
    }

    // We only care about the axis mapped to the function we are working on
    if (_rgFunctionAxisMapping[function] != axis) {
        return;
    }
    
    if (_stickDetectAxis == _axisNoAxis) {
        // Sticks have not yet moved close enough to center
        int roughCenter = getDeadbandToggle() ? std::max(_rgAxisInfo[axis].deadband,_calRoughCenterDelta) : _calRoughCenterDelta;
        if (abs(_calCenterPoint - value) < roughCenter) {
            // Stick has moved close enough to center that we can start waiting for it to settle
            _stickDetectAxis = axis;
            _stickDetectInitialValue = value;
            _stickDetectValue = value;
            qCDebug(JoystickConfigControllerLog) << "_inputStickMin detected possible center _stickDetectAxis:_stickDetectInitialValue" << _stickDetectAxis << _stickDetectInitialValue;
        }
    } else {
        if (_stickSettleComplete(axis, value)) {
            _advanceState();
        }
    }
}

/// @brief Resets internal calibration values to their initial state in preparation for a new calibration sequence.
void JoystickConfigController::_resetInternalCalibrationValues()
{
    // Set all raw axis to not reversed and center point values
    for (int i = 0; i < _axisCount; i++) {
        struct AxisInfo* info = &_rgAxisInfo[i];
        info->function = Joystick::maxFunction;
        info->reversed = false;
        info->deadband = 0;
        emit axisDeadbandChanged(i,info->deadband);
        info->axisMin  = JoystickConfigController::_calCenterPoint;
        info->axisMax  = JoystickConfigController::_calCenterPoint;
        info->axisTrim = JoystickConfigController::_calCenterPoint;
    }
    // Initialize attitude function mapping to function axis not set
    for (size_t i = 0; i < Joystick::maxFunction; i++) {
        _rgFunctionAxisMapping[i] = _axisNoAxis;
    }
    _signalAllAttitudeValueChanges();
}

/// @brief Sets internal calibration values from the stored settings
void JoystickConfigController::_setInternalCalibrationValuesFromSettings()
{
    Joystick* joystick = JoystickManager::instance()->activeJoystick();
    // Initialize all function mappings to not set
    for (int i = 0; i < _axisCount; i++) {
        struct AxisInfo* info = &_rgAxisInfo[i];
        info->function = Joystick::maxFunction;
    }
    
    for (size_t i = 0; i < Joystick::maxFunction; i++) {
        _rgFunctionAxisMapping[i] = _axisNoAxis;
    }
    
    for (int axis = 0; axis < _axisCount; axis++) {
        struct AxisInfo* info = &_rgAxisInfo[axis];
        Joystick::Calibration_t calibration = joystick->getCalibration(axis);
        info->axisTrim  = calibration.center;
        info->axisMin   = calibration.min;
        info->axisMax   = calibration.max;
        info->reversed  = calibration.reversed;
        info->deadband  = calibration.deadband;
        emit axisDeadbandChanged(axis,info->deadband);
        qCDebug(JoystickConfigControllerLog) << "Read settings name:axis:min:max:trim:reversed" << joystick->name() << axis << info->axisMin << info->axisMax << info->axisTrim << info->reversed;
    }
    
    for (int function = 0; function < Joystick::maxFunction; function++) {
        int paramAxis;
        paramAxis = joystick->getFunctionAxis(static_cast<Joystick::AxisFunction_t>(function));
        if(paramAxis >= 0 && paramAxis < _axisCount) {
            _rgFunctionAxisMapping[function] = paramAxis;
            _rgAxisInfo[paramAxis].function = static_cast<Joystick::AxisFunction_t>(function);
        }
    }

    _transmitterMode = joystick->getTXMode();
    _signalAllAttitudeValueChanges();
}

/// @brief Validates the current settings against the calibration rules resetting values as necessary.
void JoystickConfigController::_validateCalibration()
{
    for (int chan = 0; chan < _axisCount; chan++) {
        struct AxisInfo* info = &_rgAxisInfo[chan];
        // Validate Min/Max values. Although the axis appears as available we still may
        // not have good min/max/trim values for it. Set to defaults if needed.
        if (info->axisMin < _calValidMinValue || info->axisMax > _calValidMaxValue) {
            qCDebug(JoystickConfigControllerLog) << "_validateCalibration resetting axis" << chan;
            info->axisMin = _calDefaultMinValue;
            info->axisMax = _calDefaultMaxValue;
            info->axisTrim = info->axisMin + ((info->axisMax - info->axisMin) / 2);
        }
        switch (_rgAxisInfo[chan].function) {
            case Joystick::throttleFunction:
            case Joystick::yawFunction:
            case Joystick::rollFunction:
            case Joystick::pitchFunction:
            case Joystick::gimbalPitchFunction:
            case Joystick::gimbalYawFunction:
                // Make sure trim is within min/max
                if (info->axisTrim < info->axisMin) {
                    info->axisTrim = info->axisMin;
                } else if (info->axisTrim > info->axisMax) {
                    info->axisTrim = info->axisMax;
                }
                break;
            default:
                // Non-attitude control axis have calculated trim
                info->axisTrim = info->axisMin + ((info->axisMax - info->axisMin) / 2);
                break;
        }
    }
}

/// @brief Saves the rc calibration values to the board parameters.
void JoystickConfigController::_writeCalibration()
{
    Joystick* joystick = JoystickManager::instance()->activeJoystick();
    _validateCalibration();
    
    for (int axis = 0; axis < _axisCount; axis++) {
        Joystick::Calibration_t calibration;
        struct AxisInfo* info   = &_rgAxisInfo[axis];
        calibration.center      = info->axisTrim;
        calibration.min         = info->axisMin;
        calibration.max         = info->axisMax;
        calibration.reversed    = info->reversed;
        calibration.deadband    = info->deadband;
        joystick->setCalibration(axis, calibration);
    }
    
    // Write function mapping parameters
    for (int function = 0; function < Joystick::maxFunction; function++) {
        joystick->setFunctionAxis(static_cast<Joystick::AxisFunction_t>(function), _rgFunctionAxisMapping[function]);
    }
    
    _stopCalibration();
    _setInternalCalibrationValuesFromSettings();

    Vehicle* vehicle = MultiVehicleManager::instance()->activeVehicle();
    if (vehicle) {
        vehicle->setJoystickEnabled(true);
        vehicle->saveJoystickSettings();
    }
}

/// @brief Starts the calibration process
void JoystickConfigController::_startCalibration()
{
    _activeJoystick->setCalibrationMode(true);
    _resetInternalCalibrationValues();
    _currentStep = 0;
    _setupCurrentState();
    emit calibratingChanged();
}

/// @brief Cancels the calibration process, setting things back to initial state.
void JoystickConfigController::_stopCalibration()
{
    _currentStep = -1;
    _activeJoystick->setCalibrationMode(false);
    _setInternalCalibrationValuesFromSettings();
    _setStatusText("");
    emit calibratingChanged();
    _currentStickPositions.clear();
    _currentStickPositions  << _sticksCentered.leftX  << _sticksCentered.leftY  << _sticksCentered.rightX  << _sticksCentered.rightY;
    emit stickPositionsChanged();
}

/// @brief Saves the current axis values, so that we can detect when the use moves an input.
void JoystickConfigController::_calSaveCurrentValues()
{
	qCDebug(JoystickConfigControllerLog) << "_calSaveCurrentValues";
    for (int i = 0; i < _axisCount; i++) {
        _axisValueSave[i] = _axisRawValue[i];
    }
}

void JoystickConfigController::_setStickPositions()
{
    _sticksCentered = stSticksCentered;
    switch(_transmitterMode) {
    case 1:
        _sticksThrottleUp   = stRightStickUp;
        _sticksThrottleDown = stRightStickDown;
        _sticksYawLeft      = stLeftStickLeft;
        _sticksYawRight     = stLeftStickRight;
        _sticksRollLeft     = stRightStickLeft;
        _sticksRollRight    = stRightStickRight;
        _sticksPitchUp      = stLeftStickUp;
        _sticksPitchDown    = stLeftStickDown;
        break;
    case 2:
        _sticksThrottleUp   = stLeftStickUp;
        _sticksThrottleDown = stLeftStickDown;
        _sticksYawLeft      = stLeftStickLeft;
        _sticksYawRight     = stLeftStickRight;
        _sticksRollLeft     = stRightStickLeft;
        _sticksRollRight    = stRightStickRight;
        _sticksPitchUp      = stRightStickUp;
        _sticksPitchDown    = stRightStickDown;
        break;
    case 3:
        _sticksThrottleUp   = stRightStickUp;
        _sticksThrottleDown = stRightStickDown;
        _sticksYawLeft      = stRightStickLeft;
        _sticksYawRight     = stRightStickRight;
        _sticksRollLeft     = stLeftStickLeft;
        _sticksRollRight    = stLeftStickRight;
        _sticksPitchUp      = stLeftStickUp;
        _sticksPitchDown    = stLeftStickDown;
        break;
    case 4:
        _sticksThrottleUp   = stLeftStickUp;
        _sticksThrottleDown = stLeftStickDown;
        _sticksYawLeft      = stRightStickLeft;
        _sticksYawRight     = stRightStickRight;
        _sticksRollLeft     = stLeftStickLeft;
        _sticksRollRight    = stLeftStickRight;
        _sticksPitchUp      = stRightStickUp;
        _sticksPitchDown    = stRightStickDown;
        break;
    default:
        Q_ASSERT(false);
    }
}

bool JoystickConfigController::rollAxisReversed()
{
    if (_rgFunctionAxisMapping[Joystick::rollFunction] != _axisNoAxis) {
        return _rgAxisInfo[_rgFunctionAxisMapping[Joystick::rollFunction]].reversed;
    } else {
        return false;
    }
}

bool JoystickConfigController::pitchAxisReversed()
{
    if (_rgFunctionAxisMapping[Joystick::pitchFunction] != _axisNoAxis) {
        return _rgAxisInfo[_rgFunctionAxisMapping[Joystick::pitchFunction]].reversed;
    } else {
        return false;
    }
}

bool JoystickConfigController::yawAxisReversed()
{
    if (_rgFunctionAxisMapping[Joystick::yawFunction] != _axisNoAxis) {
        return _rgAxisInfo[_rgFunctionAxisMapping[Joystick::yawFunction]].reversed;
    } else {
        return false;
    }
}

bool JoystickConfigController::throttleAxisReversed()
{
    if (_rgFunctionAxisMapping[Joystick::throttleFunction] != _axisNoAxis) {
        return _rgAxisInfo[_rgFunctionAxisMapping[Joystick::throttleFunction]].reversed;
    } else {
        return false;
    }
}

void JoystickConfigController::setTransmitterMode(int mode)
{
    // Mode selection is disabled during calibration
    if (mode > 0 && mode <= 4 && _currentStep == -1) {
        _transmitterMode = mode;
        _setStickPositions();
        _activeJoystick->setTXMode(mode);
        _setInternalCalibrationValuesFromSettings();
    }
}

void JoystickConfigController::_signalAllAttitudeValueChanges()
{
    emit rollAxisMappedChanged(rollAxisMapped());
    emit pitchAxisMappedChanged(pitchAxisMapped());
    emit yawAxisMappedChanged(yawAxisMapped());
    emit throttleAxisMappedChanged(throttleAxisMapped());

    emit rollAxisReversedChanged(rollAxisReversed());
    emit pitchAxisReversedChanged(pitchAxisReversed());
    emit yawAxisReversedChanged(yawAxisReversed());
    emit throttleAxisReversedChanged(throttleAxisReversed());

    emit transmitterModeChanged(_transmitterMode);
}

void JoystickConfigController::_activeJoystickChanged(Joystick* joystick)
{
    bool joystickTransition = false;
    if (_activeJoystick) {
        joystickTransition = true;
        disconnect(_activeJoystick, &Joystick::rawAxisValueChanged, this, &JoystickConfigController::_axisValueChanged);
        // This will reset _rgFunctionAxis values to -1 to prevent out-of-bounds accesses
        _resetInternalCalibrationValues();
        delete[] _rgAxisInfo;
        delete[] _axisValueSave;
        delete[] _axisRawValue;
        _axisCount = 0;
        _activeJoystick = nullptr;
    }
    
    if (joystick) {
        _activeJoystick = joystick;
        if (joystickTransition) {
            _stopCalibration();
        }
        _activeJoystick->setCalibrationMode(false);
        _axisCount      = _activeJoystick->axisCount();
        _rgAxisInfo     = new struct AxisInfo[_axisCount];
        _axisValueSave  = new int[_axisCount];
        _axisRawValue   = new int[_axisCount];
        _setInternalCalibrationValuesFromSettings();
        connect(_activeJoystick, &Joystick::rawAxisValueChanged, this, &JoystickConfigController::_axisValueChanged);
    }
}

bool JoystickConfigController::_validAxis(int axis) const
{
    return axis >= 0 && axis < _axisCount;
}

void JoystickConfigController::_setStatusText(const QString& text)
{
    _statusText = text;
    emit statusTextChanged();
}
