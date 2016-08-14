#!/usr/bin/env ruby
require 'time'
require 'ostruct'

$config = OpenStruct.new
$config.chimeEveryNSeconds = 300
$config.chimeOffsetSeconds = 0
$config.chimeCycleSeconds = 6
$config.chimeNumber = 1
$config.chimeCount = 4
$config.interChimeDelaySeconds = 24
$config.interHourChimeDelaySeconds = 6

CHIME_INITIAL = 0
CHIME_SECOND = 1
CHIME_THIRD = 2
CHIME_HOUR = 3


def secondsTilNextN(start, n)
  # when should be a Time object.
  (((n - (start.min % n)) - 1) * 60) + (60 - start.sec)
end

def scheduleChimeSequence(now)
  chimeSchedule = []
  nowTime = now.to_i
  twelveHour = (now.hour % 12 == 0 ? 12 : now.hour % 12)

  globalFirstChimeTime = nowTime + secondsTilNextN(now, $config.chimeEveryNSeconds/60)

  (CHIME_INITIAL..CHIME_HOUR).each do |chimeState|
    cycleOffsetSeconds = ((chimeState * $config.chimeCount) + (chimeState == CHIME_HOUR ? 0 : ($config.chimeNumber-1))) * $config.chimeCycleSeconds
    if chimeState == CHIME_HOUR
      (1..twelveHour).each do |hour|
        chimeSchedule << globalFirstChimeTime + cycleOffsetSeconds + ($config.interHourChimeDelaySeconds * (hour-1))
      end
    else
      chimeSchedule << globalFirstChimeTime + cycleOffsetSeconds
    end
  end
  chimeSchedule
end

twelveoclock = scheduleChimeSequence(Time.now)
$config.chimeNumber += 1
sixoclock = scheduleChimeSequence(Time.now)
$config.chimeNumber += 1
nineoclock = scheduleChimeSequence(Time.now)
$config.chimeNumber += 1
threeoclock = scheduleChimeSequence(Time.now)

printf "%16s " + ["%16s " * 4].join("\\t") + "\n", "Chime", "Twelve", "Six", "Nine", "Three"
printf "%16s " + ["%16s " * 4].join("\\t") + "\n", "-" * 16, *(["-" * 16] * 4)
(0..twelveoclock.length-1).each do |i|
  if i == 0
    printf "%16s ", "Initial"
  elsif i == 1
    printf "%16s ", "Second"
  elsif i == 2
    printf "%16s ", "Third"
  else
    printf "%16s ", "Hour"
  end
  
  printf "%16s ", Time.at(twelveoclock[i]).strftime("%H:%M:%S")
  printf "%16s ", Time.at(sixoclock[i]).strftime("%H:%M:%S")
  printf "%16s ", Time.at(nineoclock[i]).strftime("%H:%M:%S")
  printf "%16s ", Time.at(threeoclock[i]).strftime("%H:%M:%S")
  puts
end
